#pragma once
#include <optional>
#include <perfcpp/counter/counter.hpp>

#include <cstdint>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

#if !(defined(__x86_64__) || defined(__i386__))
#define __builtin_cpu_is(x) 0
#endif

namespace perf {

/**
 * Access to information about the underlying hardware substrate like manufacturer and perf specifics.
 */
class HardwareInfo
{
public:
  class AMDInstructionBasedSampling
  {
  public:
    explicit AMDInstructionBasedSampling(const bool is_supported) noexcept
      : _is_supported(is_supported)
    {
    }

    AMDInstructionBasedSampling(AMDInstructionBasedSampling&&) noexcept = default;
    AMDInstructionBasedSampling(const AMDInstructionBasedSampling&) = default;

    ~AMDInstructionBasedSampling() noexcept = default;

    AMDInstructionBasedSampling& operator=(AMDInstructionBasedSampling&&) noexcept = default;
    AMDInstructionBasedSampling& operator=(const AMDInstructionBasedSampling&) = default;

    /**
     * @return True, if IBS is supported on the underlying hardware.
     */
    [[nodiscard]] bool is_supported() const noexcept { return _is_supported; }

    /**
     * @return The PMU type of the IBS Op device.
     */
    [[nodiscard]] std::optional<std::uint32_t> op_type() const noexcept { return _op_type; }

    /**
     * @param op_type The PMU type of the IBS Op device.
     */
    void op_type(const std::uint32_t op_type) noexcept { _op_type = op_type; }

    /**
     * @return The bit position for the uops trigger in the IBS Op config.
     */
    [[nodiscard]] std::optional<std::uint8_t> op_uops_bit() const noexcept { return _op_uops_bit; }

    /**
     * @param op_uops_bit The bit position for the uops trigger in the IBS Op config.
     */
    void op_uops_bit(const std::uint8_t op_uops_bit) noexcept { _op_uops_bit = op_uops_bit; }

    /**
     * @return The bit position for the L3 miss only filter in the IBS Op config.
     */
    [[nodiscard]] std::optional<std::uint8_t> op_l3_miss_only_bit() const noexcept { return _op_l3_miss_only_bit; }

    /**
     * @param op_l3_miss_only_bit The bit position for the L3 miss only filter in the IBS Op config.
     */
    void op_l3_miss_only_bit(const std::uint8_t op_l3_miss_only_bit) noexcept
    {
      _op_l3_miss_only_bit = op_l3_miss_only_bit;
    }

    /**
     * @return The PMU type of the IBS Fetch device.
     */
    [[nodiscard]] std::optional<std::uint32_t> fetch_type() const noexcept { return _fetch_type; }

    /**
     * @param fetch_type The PMU type of the IBS Fetch device.
     */
    void fetch_type(const std::uint32_t fetch_type) noexcept { _fetch_type = fetch_type; }

    /**
     * @return The bit position for the randomization enable in the IBS Fetch config.
     */
    [[nodiscard]] std::optional<std::uint8_t> fetch_rand_bit() const noexcept { return _fetch_rand_bit; }

    /**
     * @param fetch_rand_bit The bit position for the randomization enable in the IBS Fetch config.
     */
    void fetch_rand_bit(const std::uint8_t fetch_rand_bit) noexcept { _fetch_rand_bit = fetch_rand_bit; }

    /**
     * @return The bit position for the L3 miss only filter in the IBS Fetch config.
     */
    [[nodiscard]] std::optional<std::uint8_t> fetch_l3_miss_only_bit() const noexcept
    {
      return _fetch_l3_miss_only_bit;
    }

    /**
     * @param fetch_l3_miss_only_bit The bit position for the L3 miss only filter in the IBS Fetch config.
     */
    void fetch_l3_miss_only_bit(const std::uint8_t fetch_l3_miss_only_bit) noexcept
    {
      _fetch_l3_miss_only_bit = fetch_l3_miss_only_bit;
    }

  private:
    /// True, if IBS is supported.
    bool _is_supported;

    /// PMU type of the IBS Op device.
    std::optional<std::uint32_t> _op_type{ std::nullopt };
    /// Bit position for the uops trigger in the IBS Op config.
    std::optional<std::uint8_t> _op_uops_bit{ std::nullopt };
    /// Bit position for the L3 miss only filter in the IBS Op config.
    std::optional<std::uint8_t> _op_l3_miss_only_bit{ std::nullopt };

    /// PMU type of the IBS Fetch device.
    std::optional<std::uint32_t> _fetch_type{ std::nullopt };
    /// Bit position for the randomization enable in the IBS Fetch config.
    std::optional<std::uint8_t> _fetch_rand_bit{ std::nullopt };
    /// Bit position for the L3 miss only filter in the IBS Fetch config.
    std::optional<std::uint8_t> _fetch_l3_miss_only_bit{ std::nullopt };
  };

  /**
   * @return A vector containing the logical ids of all CPU cores.
   */
  [[nodiscard]] static std::vector<std::uint16_t> all_cpu_cores();

  /**
   * @return True, if the underlying hardware is an Intel processor.
   */
  [[nodiscard]] static bool is_intel() noexcept { return __builtin_cpu_is("intel"); }

  /**
   * @return True, if the underlying Intel processor requires an aux counter for memory sampling.
   */
  [[nodiscard]] static bool is_intel_aux_counter_required();

  /**
   * @return True, if the underlying Intel processor is equal or newer than the 12th generation.
   */
  [[nodiscard]] static bool is_intel_12th_generation_or_newer();

  /**
   * @return True, if the underlying hardware is an AMD processor.
   */
  [[nodiscard]] static bool is_amd() noexcept { return __builtin_cpu_is("amd"); }

  /**
   * @return Information about AMD Instruction Based Sampling if the underlying hardware is an AMD processor.
   */
  [[nodiscard]] static const AMDInstructionBasedSampling& amd_ibs();

  /**
   * @return True, if the underlying AMD processor supports Instruction Based Sampling (IBS).
   */
  [[nodiscard]] static bool is_amd_ibs_supported();

  /**
   * @return The page size of memory of the underlying machine.
   */
  [[nodiscard]] static std::uint64_t memory_page_size();

  /**
   * @return The number of generic (programmable) physical performance counters per logical CPU core.
   */
  [[nodiscard]] static std::uint8_t physical_generic_performance_counters_per_logical_core();

  /**
   * @return The number of fixed-function physical performance counters per logical CPU core.
   *         Fixed counters are dedicated to specific events (e.g., instructions, cycles, ref-cycles on Intel).
   *         Returns 0 on platforms without fixed counters (AMD, ARM).
   */
  [[nodiscard]] static std::uint8_t physical_fixed_performance_counters_per_logical_core();

  /**
   * @return The number of events that can be scheduled to the same physical performance counter.
   */
  [[nodiscard]] static std::uint8_t events_per_physical_performance_counter();

  /**
   * @return The maximum clock frequency across all cores in Hz.
   */
  [[nodiscard]] static std::uint64_t max_cpu_clock_frequency();

  /**
   * @return True, if the NMI watchdog is enabled and permanently consumes one hw-PMU counter.
   */
  [[nodiscard]] static bool is_nmi_watchdog_enabled();

private:
  static std::optional<bool> _is_nmi_watchdog_enabled;
  static std::optional<bool> _is_intel_aux_event_required;
  static std::optional<bool> _is_intel_12th_generation_or_newer;
  static std::optional<AMDInstructionBasedSampling> _amd_ibs;
  static std::optional<std::uint64_t> _memory_page_size;
  static std::optional<std::uint8_t> _physical_generic_performance_counters_per_logical_core;
  static std::optional<std::uint8_t> _physical_fixed_performance_counters_per_logical_core;
  static std::optional<std::uint8_t> _events_per_physical_performance_counter;
  static std::optional<std::uint64_t> _max_cpu_clock_frequency;

#if defined(__x86_64__) || defined(__i386__)
  /**
   * Result of a __get_cpuid call.
   */
  class CPUIDResult
  {
  public:
    CPUIDResult() noexcept = default;
    ~CPUIDResult() noexcept = default;

    CPUIDResult(const CPUIDResult&) noexcept = default;
    CPUIDResult(CPUIDResult&&) noexcept = default;

    [[nodiscard]] CPUIDResult& operator=(const CPUIDResult&) noexcept = default;
    [[nodiscard]] CPUIDResult& operator=(CPUIDResult&&) noexcept = default;

    std::uint32_t eax;
    std::uint32_t ebx;
    std::uint32_t ecx;
    std::uint32_t edx;
  };

  /**
   * Fires a __get_cpuid call with the provided leaf and sub leaf. In case the call was successful, the register values
   * are returned.
   *
   * @param leaf Leaf.
   * @param sub_leaf Sub leaf (0 by default).
   * @return Register values (eax, ebx, ecx, edx) in case the cpuid request was successful.
   */
  static std::optional<CPUIDResult> cpuid(std::uint32_t leaf, std::uint32_t sub_leaf = 0U) noexcept;
#endif

  /**
   * Writes a value into the cache variable and returns the value.
   *
   * @param variable Cache variable.
   * @param value Value to write into the cache variable.
   * @return The cached value.
   */
  template<typename T>
  [[nodiscard]] static T cache_value(std::optional<T>& variable, const T value)
  {
    variable = value;
    return value;
  }

  /**
   * Tries to open a performance counter with more and more events until it cannot open more events on a single physical
   * performance counter.
   *
   * @param is_identify_hardware_counters If true, identify the number of hardware counters. Otherwise, identify the
   * number of events per hardware counter.
   * @return The maximum number of events on a single physical performance counter.
   */
  [[nodiscard]] static std::optional<std::uint8_t> explore_hardware_counters_experimentally(
    bool is_identify_hardware_counters);

  /**
   * Creates a list for hardware counter and event identification. The list may depend on the underlying hardware (e.g.,
   * some ARM CPUs do not support all events defined by the perf subsystem).
   *
   * @return List of events to experiment for hardware counter and event identification.
   */
  [[nodiscard]] static std::vector<CounterConfig> generate_events_for_counter_identification();
};
}