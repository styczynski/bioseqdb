#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <linux/perf_event.h>
#include <perfcpp/counter/requested_event.hpp>
#include <perfcpp/feature.h>
#include <perfcpp/sample/ibs_decoder.hpp>
#include <perfcpp/sample/metadata.hpp>
#include <perfcpp/sample/recording_values.hpp>
#include <perfcpp/sampler.hpp>
#include <utility>
#include <vector>

namespace perf {
/**
 * The UserLevelBufferEntry represents an entry in the user-level buffer filled by the perf subsystem by parsing the
 * hardware-related samples. This helper assists in consuming data from the buffer and turning it into Samples.
 */
class SampleIterator
{
public:
  explicit SampleIterator(const std::uintptr_t address) noexcept
    : _header(reinterpret_cast<perf_event_header*>(address))
    , _data(address + sizeof(perf_event_header))
  {
  }

  SampleIterator(const SampleIterator&) = default;
  SampleIterator(SampleIterator&& other) noexcept
    : _header(std::exchange(other._header, nullptr))
    , _data(std::exchange(other._data, 0ULL))
  {
  }

  ~SampleIterator() noexcept = default;

  SampleIterator& operator=(const SampleIterator&) = default;
  SampleIterator& operator=(SampleIterator&& other) noexcept
  {
    _header = std::exchange(other._header, nullptr);
    _data = std::exchange(other._data, 0ULL);
    return *this;
  }

  [[nodiscard]] std::optional<Metadata::Mode> mode() const noexcept;
  [[nodiscard]] std::uint16_t size() const noexcept { return _header->size; }

  template<typename T>
  [[nodiscard]] T read() noexcept
  {
    T data{};
    std::memcpy(&data, reinterpret_cast<const T*>(_data), sizeof(T));

    _data += sizeof(T);

    return data;
  }

  template<typename T>
  [[nodiscard]] const T* read_array(const std::size_t size) noexcept
  {
    if (size == 0U) {
      return nullptr;
    }

    auto* begin = reinterpret_cast<T*>(_data);
    _data += sizeof(T) * size;

    return begin;
  }

  template<typename T>
  void skip() noexcept
  {
    _data += sizeof(T);
  }

  template<typename T>
  void skip(const std::size_t size) noexcept
  {
    _data += sizeof(T) * size;
  }

  template<typename T>
  [[nodiscard]] T as() const noexcept
  {
    return reinterpret_cast<T>(_data);
  }

  [[nodiscard]] bool is_sample_event() const noexcept { return _header->type == PERF_RECORD_SAMPLE; }
  [[nodiscard]] bool is_lost_samples_event() const noexcept
  {
#ifndef PERFCPP_NO_RECORD_LOST_SAMPLES /// PERF_RECORD_LOST_SAMPLES is supported since Linux 4.2
    return _header->type == PERF_RECORD_LOST_SAMPLES;
#else
    return false;
#endif
  }
  [[nodiscard]] bool is_lost_event() const noexcept { return _header->type == PERF_RECORD_LOST; }
  [[nodiscard]] bool is_context_switch_event() const noexcept
  {
#ifndef PERFCPP_NO_RECORD_SWITCH /// Switch events are supported since Linux 4.3
    return _header->type == PERF_RECORD_SWITCH || _header->type == PERF_RECORD_SWITCH_CPU_WIDE;
#else
    return false;
#endif
  }
  [[nodiscard]] bool is_context_switch_cpu_wide() const noexcept
  {
#ifndef PERFCPP_NO_RECORD_SWITCH /// Switch events are supported since Linux 4.3
    return _header->type == PERF_RECORD_SWITCH_CPU_WIDE;
#else
    return false;
#endif
  }
  [[nodiscard]] bool is_cgroup_event() const noexcept
  {
#ifndef PERFCPP_NO_RECORD_CGROUP /// cgroup events is supported since Linux 5.7
    return _header->type == PERF_RECORD_CGROUP;
#else
    return false;
#endif
  }
  [[nodiscard]] bool is_throttle_event() const noexcept
  {
    return _header->type == PERF_RECORD_THROTTLE || _header->type == PERF_RECORD_UNTHROTTLE;
  }
  [[nodiscard]] bool is_throttle() const noexcept { return _header->type == PERF_RECORD_THROTTLE; }

  [[nodiscard]] bool is_instruction_pointer_exact() const noexcept
  {
    return static_cast<bool>(_header->misc & PERF_RECORD_MISC_EXACT_IP);
  }
  [[nodiscard]] bool is_context_switch_out() const noexcept
  {
#ifndef PERFCPP_NO_RECORD_SWITCH /// Switch events are supported since Linux 4.3
    return static_cast<bool>(_header->misc & PERF_RECORD_MISC_SWITCH_OUT);
#else
    return false;
#endif
  }
  [[nodiscard]] bool is_context_switch_out_preempt() const noexcept
  {
#ifndef PERFCPP_NO_RECORD_MISC_SWITCH_OUT_PREEMPT /// Preempt flag of switch events is supported since Linux 4.3
    return static_cast<bool>(_header->misc & PERF_RECORD_MISC_SWITCH_OUT_PREEMPT);
#else
    return false;
#endif
  }

private:
  /// Header of the event.
  perf_event_header* _header;

  /// Data (located directly after the header).
  std::uintptr_t _data;
};

/**
 * The SampleDecoder translates raw values emitted by the perf subsystem into Samples.
 */
class SampleDecoder
{
public:
  SampleDecoder(const CounterDefinition& counter_definition, const SampleRecordingValues& values)
    : _counter_definition(counter_definition)
    , _sampler_values(values)
  {
  }
  SampleDecoder(const SampleDecoder&) = default;
  SampleDecoder(SampleDecoder&&) noexcept = default;
  ~SampleDecoder() noexcept = default;
  SampleDecoder& operator=(const SampleDecoder&) = delete;
  SampleDecoder& operator=(SampleDecoder&&) noexcept = delete;

  /**
   * Decodes the samples from the sample buffers and translates them into Samples.
   *
   * @param sample_buffers Buffers holding the raw data that will be decoded and translated.
   * @param has_amd_ibs_op_pmu Flag, indicating if the IBS Op PMU was used.
   * @param has_amd_ibs_fetch_pmu Flag, indicating if the IBS Fetch PMU was used.
   * @param requested_event_set List of requested events.
   * @param event_group Group of hardware events.
   * @return List of decoded samples.
   */
  [[nodiscard]] std::vector<Sample> decode(const std::vector<std::vector<std::byte>>& sample_buffers,
                                           bool has_amd_ibs_op_pmu,
                                           bool has_amd_ibs_fetch_pmu,
                                           const RequestedEventSet& requested_event_set,
                                           const Group& event_group) const;

private:
  const CounterDefinition& _counter_definition;

  /// Values (i.e., requested fields) passed to the sampler when initializing.
  const SampleRecordingValues& _sampler_values;

  /**
   * Reads the sample_id struct from the data located at sample_ptr into the provided sample.
   * Corresponds to sample_id emitted when sample_id_all is set.
   *
   * @param entry Entry from the sample iterator.
   * @param sample Sample to read the data into.
   */
  void decode_sample_id_all(SampleIterator& entry, Sample& sample) const noexcept;

  /**
   * Translates the current entry from the user-level buffer into a "normal" sample.
   *
   * @param entry Entry of the user-level buffer.
   * @param has_amd_ibs_op_pmu Flag indicating if the AMD IBS Op PMU is the trigger.
   * @param has_amd_ibs_fetch_pmu Flag, indicating if the AMD IBS Fetch PMU is the trigger.
   * @param requested_event_set Set of requested events
   * @param event_group Group of hardware events.
   * @return Sample.
   */
  [[nodiscard]] perf::Sample decode_sample_event(SampleIterator&& entry,
                                                 bool has_amd_ibs_op_pmu,
                                                 bool has_amd_ibs_fetch_pmu,
                                                 const RequestedEventSet& requested_event_set,
                                                 const Group& event_group) const;

  /**
   * Reads registers from the current buffer entry.
   *
   * @param entry Current position at the buffer.
   * @param registers Set registers.
   * @return Register values.
   */
  [[nodiscard]] static RegisterValues decode_registers(SampleIterator& entry, const Registers& registers);

  /**
   * Reads hardware events from the current buffer entry.
   *
   * @param entry Current position at the buffer.
   * @param requested_event_set Set of requested events
   * @param event_group Group of hardware events.
   * @return Event values
   */
  [[nodiscard]] std::optional<CounterResult> decode_hardware_events_values(SampleIterator& entry,
                                                                           const RequestedEventSet& requested_event_set,
                                                                           const Group& event_group) const;

  /**
   * Reads the callchain from the current buffer entry.
   *
   * @param entry Current position at the buffer.
   * @return List of instruction pointers (the callchain).
   */
  [[nodiscard]] static std::optional<std::vector<std::uintptr_t>> decode_callchain(SampleIterator& entry);

  /**
   * Reads the branch stack from the current buffer entry.
   *
   * @param entry Current position at the buffer.
   * @return Branch stack.
   */
  [[nodiscard]] static std::optional<std::vector<Branch>> decode_branch_stack(SampleIterator& entry);

  /**
   * Decodes and sets the data access and/or instruction latency (based on the underlying hardware) from a single
   * latency value (coming from PERF_SAMPLE_WEIGHT).
   *
   * @param latency Latency value to decode.
   * @param sample Sample to write the latency to.
   */
  void decode_latency(std::uint32_t latency, Sample& sample) const noexcept;

  /**
   * Decodes and sets the data access and/or instruction latency (based on the underlying hardware) from multiple
   * latency values (coming from PERF_SAMPLE_WEIGHT_STRUCT).
   *
   * @param latency Latency value to decode.
   * @param sample Sample to write the latency to.
   */
  void decode_latency(std::tuple<std::uint32_t, std::uint16_t, std::uint16_t> latency, Sample& sample) const noexcept;

  /**
   * Decodes the data source and writes the data into the provided sample.
   *
   * @param data_source Perf data source to decode.
   * @param sample Sample to write the results to.
   */
  void decode_data_access(perf_mem_data_src data_source, Sample& sample) const;

  /**
   * Translates the perf data source information into an AccessType.
   *
   * @param perf_data_source Data source read from perf sample.
   * @return Translated access type.
   */
  [[nodiscard]] static std::optional<DataAccess::AccessType> decode_data_access_type(
    perf_mem_data_src perf_data_source) noexcept;

  /**
   * Checks if the data source contains an access type.
   *
   * @param perf_data_source Data source read from perf sample.
   * @return True, if the data source contains an access type.
   */
  [[nodiscard]] static bool has_data_access_type(perf_mem_data_src perf_data_source) noexcept;

  /**
   * Translates the perf data source information into a Source.
   *
   * @param perf_data_source Data source read from perf sample.
   * @return Translated source.
   */
  [[nodiscard]] static std::optional<DataAccess::Source> decode_data_access_source_and_remote(
    perf_mem_data_src perf_data_source) noexcept;

  /**
   * Reads the access source and translates it into a Source.
   *
   * @param memory_level_code Memory access code provided by the perf subsystem.
   * @return Translated source.
   */
  [[nodiscard]] static DataAccess::Source decode_data_access_source(std::uint64_t memory_level_code) noexcept;

  /**
   * Reads the number of (remote) hops and translates it into a single integer.
   *
   * @param hops_code Hops provided by the perf subsystem. Unused, when perf provides only the level code.
   * @param memory_level_code Memory level code provided by the perf subsystem. Unused, when perf provides the hops
   * code.
   * @return Number of hops.
   */
  [[nodiscard]] static std::optional<std::uint8_t> decode_data_access_remote_hops(
    [[maybe_unused]] std::uint64_t hops_code,
    [[maybe_unused]] std::uint64_t memory_level_code) noexcept;

  /**
   * Reads the snoop information and translates it into a data access Snoop.
   *
   * @param snoop_code Snoop code provided by the perf subsystem.
   * @param snoopx_code Extended snoop code provided by the perf subsystem.
   * @return Translated snoop object.
   */
  [[nodiscard]] static std::optional<DataAccess::Snoop> decode_data_access_snoop(std::uint64_t snoop_code,
                                                                                 std::uint64_t snoopx_code) noexcept;

  /**
   * Read the TLB information and translates into a pair (dTLB hit, STLB hit).
   *
   * @param tlb_code TLB code provided by the perf subsystem.
   * @return Translated pair (dTLB hit, STLB hit).
   */
  [[nodiscard]] static std::optional<std::pair<bool, bool>> decode_data_access_tlb(std::uint64_t tlb_code) noexcept;

  /**
   * Reads the perf lock information from a sample and translates it into a bool
   *
   * @param lock Memory lock code provided by the perf subsystem.
   * @return Translated lock.
   */
  [[nodiscard]] static std::optional<bool> decode_data_access_is_locked(std::uint64_t lock) noexcept;

  /**
   * Translates the abort code.
   *
   * @param abort Code of the abort.
   * @return Hardware transaction abort.
   */
  [[nodiscard]] static InstructionExecution::HardwareTransactionAbort decode_hardware_transaction_abort(
    std::uint64_t abort) noexcept;

  /**
   * Enriches the given sample with information that is present in the IBS Fetch PMU raw data but cannot be accessed by
   * the perf subsystem interface.
   *
   * @param sample Sample to enrich.
   * @param raw_values Raw data to decode.
   */
  void enrich_sample_with_ibs_fetch_data_from_raw(Sample& sample,
                                                  const std::vector<std::byte>& raw_values) const noexcept;

  /**
   * Enriches the given sample with information that is present in the IBS Op PMU raw data but cannot be accessed by the
   * perf subsystem interface.
   *
   * @param sample Sample to enrich.
   * @param raw_values Raw data to decode.
   */
  void enrich_sample_with_ibs_op_data_from_raw(Sample& sample, const std::vector<std::byte>& raw_values) const noexcept;

  /**
   * Translates branch information from the decoder into a branch type.
   *
   * @param ibs_op_decoder IBS Op Decoder containing the relevant information.
   * @return Branch type.
   */
  [[nodiscard]] static std::optional<InstructionExecution::BranchType> decode_branch_type(
    const IBSOpDecoder& ibs_op_decoder) noexcept;

  /**
   * Translates the TLB page size in a number of bytes, based on the options.
   *
   * @param is_1g True, if the page is 1GB.
   * @param is_2m True, if the page is 2MB.
   * @return The size in bytes.
   */
  [[nodiscard]] static std::uint64_t decode_tlb_page_size(bool is_1g, bool is_2m) noexcept;

  /**
   * Translates the TLB page size in a number of bytes, based on the options.
   *
   * @param code Code for the TLB page size.
   * @return The size in bytes.
   */
  [[nodiscard]] static std::optional<std::uint64_t> decode_tlb_page_size(std::uint8_t code) noexcept;

  /**
   * Translates the current entry from the user-level buffer into a lost sample.
   *
   * @param entry Entry of the user-level buffer.
   * @param has_leading_even_id If set, the entry contains the event ID field.
   * @return Sample containing the loss.
   */
  [[nodiscard]] Sample decode_lost_samples_event(SampleIterator&& entry, bool has_leading_even_id) const noexcept;

  /**
   * Translates the current entry from the user-level buffer into a context switch sample.
   *
   * @param entry Entry of the user-level buffer.
   * @return Sample containing the context switch.
   */
  [[nodiscard]] Sample decode_context_switch_event(SampleIterator&& entry) const noexcept;

  /**
   * Translates the current entry from the user-level buffer into a cgroup sample.
   *
   * @param entry Entry of the user-level buffer.
   * @return Sample containing the cgroup.
   */
  [[nodiscard]] Sample decode_cgroup_event(SampleIterator&& entry) const;

  /**
   * Translates the current entry from the user-level buffer into a throttle or un-throttle sample.
   *
   * @param entry Entry of the user-level buffer.
   * @return Sample containing the throttle.
   */
  [[nodiscard]] Sample decode_throttle_event(SampleIterator&& entry) const noexcept;
};
}