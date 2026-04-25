#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <perfcpp/counter/group.hpp>
#include <perfcpp/counter_definition.hpp>
#include <perfcpp/event_file_descriptor_parser.hpp>
#include <perfcpp/hardware_info.hpp>
#include <thread>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

/// Cache variable to remember if Intel's auxiliary event is required for sampling.
std::optional<bool> perf::HardwareInfo::_is_intel_aux_event_required{ std::nullopt };

/// Cache variable to remember if the underlying Intel hardware is from the 12th generation or even newer; these
/// machines have heterogeneous CPUs and PMUs.
std::optional<bool> perf::HardwareInfo::_is_intel_12th_generation_or_newer{ std::nullopt };

/// Cache variable to remember AMD IBS information.
std::optional<perf::HardwareInfo::AMDInstructionBasedSampling> perf::HardwareInfo::_amd_ibs{ std::nullopt };

/// Cache variable to remember the memory page size.
std::optional<std::uint64_t> perf::HardwareInfo::_memory_page_size{ std::nullopt };

/// Number of generic (programmable) performance counters per logical CPU core.
std::optional<std::uint8_t> perf::HardwareInfo::_physical_generic_performance_counters_per_logical_core{ std::nullopt };

/// Number of fixed-function performance counters per logical CPU core.
std::optional<std::uint8_t> perf::HardwareInfo::_physical_fixed_performance_counters_per_logical_core{ std::nullopt };

/// Number of events that can be scheduled to the same physical performance counter.
std::optional<std::uint8_t> perf::HardwareInfo::_events_per_physical_performance_counter{ std::nullopt };

/// Maximal clock frequency across all cores in Hz.
std::optional<std::uint64_t> perf::HardwareInfo::_max_cpu_clock_frequency{ std::nullopt };

/// Cache variable to remember if NMI watchdog is enabled.
std::optional<bool> perf::HardwareInfo::_is_nmi_watchdog_enabled{ std::nullopt };

std::vector<std::uint16_t>
perf::HardwareInfo::all_cpu_cores()
{
  auto cpu_cores = std::vector<std::uint16_t>(std::thread::hardware_concurrency());
  std::iota(cpu_cores.begin(), cpu_cores.end(), 0U);

  return cpu_cores;
}

bool
perf::HardwareInfo::is_intel_aux_counter_required()
{
#if defined(__x86_64__) || defined(__i386__)
  if (HardwareInfo::_is_intel_aux_event_required.has_value()) {
    return HardwareInfo::_is_intel_aux_event_required.value();
  }

  if (!HardwareInfo::is_intel()) {
    return HardwareInfo::cache_value(HardwareInfo::_is_intel_aux_event_required, false);
  }

  const auto is_aux_event_required =
    std::filesystem::exists(std::filesystem::path("/sys/bus/event_source/devices/cpu/events/mem-loads-aux")) ||
    std::filesystem::exists(std::filesystem::path("/sys/bus/event_source/devices/cpu_core/events/mem-loads-aux"));
  return HardwareInfo::cache_value(HardwareInfo::_is_intel_aux_event_required, is_aux_event_required);
#else
  return false;
#endif
}

bool
perf::HardwareInfo::is_intel_12th_generation_or_newer()
{
#if defined(__x86_64__) || defined(__i386__)
  if (HardwareInfo::_is_intel_12th_generation_or_newer.has_value()) {
    return HardwareInfo::_is_intel_12th_generation_or_newer.value();
  }

  if (!HardwareInfo::is_intel()) {
    return HardwareInfo::cache_value(HardwareInfo::_is_intel_12th_generation_or_newer, false);
  }

  /// Get processor family/model information
  if (const auto model_info = HardwareInfo::cpuid(0x1); model_info.has_value()) {
    /// Check the family.
    const auto family_id = (model_info->eax >> 8) & 0xF;
    const auto extended_family_id = (model_info->eax >> 20) & 0xFF;

    /// Families < 6 are older than Alder Lake (12th generation); families > 6 are newer (and do not exist up to now).
    if (const auto display_family = family_id + extended_family_id;
        display_family != /* 6U is the line between 12th and earlier generations */ 6U) {
      return HardwareInfo::cache_value(HardwareInfo::_is_intel_12th_generation_or_newer, display_family > 6U);
    }

    /// For family 6, check the model.
    const auto model = (model_info->eax >> 4) & 0xF;
    const auto extended_model = (model_info->eax >> 16) & 0xF;

    const auto display_model = (extended_model << 4) + model;
    return HardwareInfo::cache_value(HardwareInfo::_is_intel_12th_generation_or_newer,
                                     display_model >= /* 12th generation */ 143U);
  }

  return HardwareInfo::cache_value(HardwareInfo::_is_intel_12th_generation_or_newer, false);

#else
  return false;
#endif
}

const perf::HardwareInfo::AMDInstructionBasedSampling&
perf::HardwareInfo::amd_ibs()
{
#if defined(__x86_64__) || defined(__i386__)
  if (HardwareInfo::_amd_ibs.has_value()) {
    return HardwareInfo::_amd_ibs.value();
  }

  /// Check if the hardware underneath is AMD.
  if (!HardwareInfo::is_amd()) {
    HardwareInfo::_amd_ibs = AMDInstructionBasedSampling{ false };
    return HardwareInfo::_amd_ibs.value();
  }

  /// If the hardware is AMD, check if IBS is supported.
  /// See https://github.com/jlgreathouse/AMD_IBS_Toolkit/blob/master/ibs_with_perf_events.txt
  const auto extended_processor_info = HardwareInfo::cpuid(0x80000001);
  if (!extended_processor_info.has_value()) {
    HardwareInfo::_amd_ibs = AMDInstructionBasedSampling{ false };
    return HardwareInfo::_amd_ibs.value();
  }

  if (const auto is_ibs_supported = static_cast<bool>(extended_processor_info->ecx & (1U << 10)); !is_ibs_supported) {
    HardwareInfo::_amd_ibs = AMDInstructionBasedSampling{ false };
    return HardwareInfo::_amd_ibs.value();
  }

  auto ibs_info = AMDInstructionBasedSampling{ true };

  /// Check if L3Miss filter is supported.
  auto is_l3miss_filter_supported = false;
  if (const auto ibs_l3miss_info = HardwareInfo::cpuid(0x8000001b); ibs_l3miss_info.has_value()) {
    is_l3miss_filter_supported = static_cast<bool>(ibs_l3miss_info->eax & (1U << 11));
  }

  /// Read IBS::Fetch from filesystem.
  {
    auto fetch_event = EventFileDescriptorParser{ "/sys/bus/event_source/devices/ibs_fetch/" };
    if (const auto fetch_event_type = fetch_event.type(); fetch_event_type.has_value()) {
      ibs_info.fetch_type(fetch_event_type.value());

      /// Read rand_en bit.
      if (const auto fetch_rand_bit = fetch_event.format("rand_en"); fetch_rand_bit.size() == 1UL) {
        ibs_info.fetch_rand_bit(std::get<0U>(std::get<1U>(fetch_rand_bit.front())));
      }

      /// Read l3_miss_only bit.
      if (is_l3miss_filter_supported) {
        if (const auto fetch_l3miss_bit_format = fetch_event.format("l3missonly");
            fetch_l3miss_bit_format.size() == 1UL) {
          ibs_info.fetch_l3_miss_only_bit(std::get<0U>(std::get<1U>(fetch_l3miss_bit_format.front())));
        }
      }
    }
  }

  /// Read IBS::Op from filesystem.
  {
    auto op_event = EventFileDescriptorParser{ "/sys/bus/event_source/devices/ibs_op/" };

    if (const auto op_event_type = op_event.type(); op_event_type.has_value()) {
      ibs_info.op_type(op_event_type.value());

      /// Read uop bit.
      if (const auto op_cnt_bit = op_event.format("cnt_ctl"); op_cnt_bit.size() == 1UL) {
        ibs_info.op_uops_bit(std::get<0U>(std::get<1U>(op_cnt_bit.front())));
      }

      /// Read l3_miss_only bit.
      if (is_l3miss_filter_supported) {
        if (const auto op_l3miss_bit_format = op_event.format("l3missonly"); op_l3miss_bit_format.size() == 1UL) {
          ibs_info.op_l3_miss_only_bit(std::get<0U>(std::get<1U>(op_l3miss_bit_format.front())));
        }
      }
    }
  }

  HardwareInfo::_amd_ibs = ibs_info;
#else
  HardwareInfo::_amd_ibs = AMDInstructionBasedSampling{ false };
#endif

  return HardwareInfo::_amd_ibs.value();
}

bool
perf::HardwareInfo::is_amd_ibs_supported()
{
  const auto& amd_ibs = HardwareInfo::amd_ibs();
  return amd_ibs.is_supported();
}

std::uint64_t
perf::HardwareInfo::memory_page_size()
{
  if (HardwareInfo::_memory_page_size.has_value()) {
    return HardwareInfo::_memory_page_size.value();
  }

  /// Read memory page size from sysconf (see https://man7.org/linux/man-pages/man3/sysconf.3.html).
  const auto memory_page_size = static_cast<std::uint64_t>(std::max(0L, ::sysconf(_SC_PAGESIZE)));
  return HardwareInfo::cache_value(HardwareInfo::_memory_page_size, memory_page_size);
}

std::uint8_t
perf::HardwareInfo::physical_generic_performance_counters_per_logical_core()
{
  if (HardwareInfo::_physical_generic_performance_counters_per_logical_core.has_value()) {
    return HardwareInfo::_physical_generic_performance_counters_per_logical_core.value();
  }

#if defined(__x86_64__) || defined(__i386__)
  if (HardwareInfo::is_intel()) {
    /// Read CPUID information with 0x0A (see https://www.felixcloutier.com/x86/cpuid).
    if (const auto pmu_info = HardwareInfo::cpuid(0x0A); pmu_info.has_value()) {
      /// Number of general-purpose performance monitoring counter per logical processor is in bits 15-08.
      const auto performance_counters_per_logical_core = (pmu_info->eax >> 8) & 0xFF;

      return HardwareInfo::cache_value(HardwareInfo::_physical_generic_performance_counters_per_logical_core,
                                       static_cast<std::uint8_t>(performance_counters_per_logical_core));
    }
  }

  if (HardwareInfo::is_amd()) {
    /// Check the Extended Processor Information (0x80000001), see
    /// http://www.flounder.com/cpuid_explorer2.htm#CPUID(0x80000001):ECX.
    if (const auto extended_processor_info = HardwareInfo::cpuid(0x80000001);
        extended_processor_info.has_value() &&
        static_cast<bool>(extended_processor_info->ecx & (static_cast<std::uint32_t>(1U) << 23))) {

      /// Check the Extended Information (0x80000000), see
      /// http://www.flounder.com/cpuid_explorer2.htm#CPUID(0x80000000).
      if (const auto extended_info = HardwareInfo::cpuid(0x80000000);
          extended_info.has_value() && extended_info->eax >= 0x80000022) {

        /// Check the Performance Monitoring Unit Information (0x80000022).
        if (const auto pmu_info = HardwareInfo::cpuid(0x80000022); pmu_info.has_value()) {
          const auto performance_counters_per_logical_core = pmu_info->eax & 0xFF;

          return HardwareInfo::cache_value(HardwareInfo::_physical_generic_performance_counters_per_logical_core,
                                           static_cast<std::uint8_t>(performance_counters_per_logical_core));
        }
      }
    }
  }
#endif

  /// Try to find the number of hardware counters per logical core.
  const auto hardware_counters = HardwareInfo::explore_hardware_counters_experimentally(true);

  /// Fallback: Set to zero, if the experiment failed.
  if (!hardware_counters.has_value()) {
    return HardwareInfo::cache_value(HardwareInfo::_physical_generic_performance_counters_per_logical_core,
                                     static_cast<std::uint8_t>(0U));
  }

  return HardwareInfo::cache_value(HardwareInfo::_physical_generic_performance_counters_per_logical_core,
                                   hardware_counters.value());
}

std::uint8_t
perf::HardwareInfo::physical_fixed_performance_counters_per_logical_core()
{
  if (HardwareInfo::_physical_fixed_performance_counters_per_logical_core.has_value()) {
    return HardwareInfo::_physical_fixed_performance_counters_per_logical_core.value();
  }

#if defined(__x86_64__) || defined(__i386__)
  if (HardwareInfo::is_intel()) {
    /// Read CPUID information with 0x0A (see https://www.felixcloutier.com/x86/cpuid).
    if (const auto pmu_info = HardwareInfo::cpuid(0x0A); pmu_info.has_value()) {
      /// Number of fixed-function performance counters is in EDX bits 4-0.
      const auto fixed_counters = pmu_info->edx & 0x1F;

      return HardwareInfo::cache_value(HardwareInfo::_physical_fixed_performance_counters_per_logical_core,
                                       static_cast<std::uint8_t>(fixed_counters));
    }
  }
#endif

  /// AMD and ARM do not have fixed-function performance counters.
  return HardwareInfo::cache_value(HardwareInfo::_physical_fixed_performance_counters_per_logical_core,
                                   static_cast<std::uint8_t>(0U));
}

#if defined(__x86_64__) || defined(__i386__)
std::optional<perf::HardwareInfo::CPUIDResult>
perf::HardwareInfo::cpuid(const std::uint32_t leaf, const std::uint32_t sub_leaf) noexcept
{

  auto result = CPUIDResult{};
  if (__get_cpuid_count(leaf, sub_leaf, &result.eax, &result.ebx, &result.ecx, &result.edx) > 0) {
    return result;
  }

  return std::nullopt;
}
#endif

std::uint8_t
perf::HardwareInfo::events_per_physical_performance_counter()
{
  if (HardwareInfo::_events_per_physical_performance_counter.has_value()) {
    return HardwareInfo::_events_per_physical_performance_counter.value();
  }

  /// Try to find the number of events per physical performance counter.
  if (const auto events_per_hardware_counter = HardwareInfo::explore_hardware_counters_experimentally(false);
      events_per_hardware_counter.has_value()) {
    return HardwareInfo::cache_value(HardwareInfo::_events_per_physical_performance_counter,
                                     events_per_hardware_counter.value());
  }

  /// Fallback: Set to one, if the experiment failed.
  return HardwareInfo::cache_value(HardwareInfo::_events_per_physical_performance_counter,
                                   static_cast<std::uint8_t>(1U));
}

bool
perf::HardwareInfo::is_nmi_watchdog_enabled()
{
  if (HardwareInfo::_is_nmi_watchdog_enabled.has_value()) {
    return HardwareInfo::_is_nmi_watchdog_enabled.value();
  }

  /// Read NMI watchdog status from procfs (see https://www.kernel.org/doc/Documentation/lockup-watchdogs.txt).
  auto watchdog_file = std::ifstream{ "/proc/sys/kernel/nmi_watchdog" };
  if (auto value = 0; watchdog_file >> value) {
    return HardwareInfo::cache_value(HardwareInfo::_is_nmi_watchdog_enabled, value != 0);
  }

  return HardwareInfo::cache_value(HardwareInfo::_is_nmi_watchdog_enabled, false);
}

std::uint64_t
perf::HardwareInfo::max_cpu_clock_frequency()
{
  if (HardwareInfo::_max_cpu_clock_frequency.has_value()) {
    return HardwareInfo::_max_cpu_clock_frequency.value();
  }

  auto max_frequency_in_hz = 0UL;

  for (const auto& entry : std::filesystem::directory_iterator("/sys/devices/system/cpu")) {
    if (entry.is_directory()) {
      if (auto cpu_directory_name = entry.path().filename().string();
          cpu_directory_name.rfind("cpu", 0U) == 0U && std::isdigit(cpu_directory_name[3U]) != 0) {
        auto freq_file = std::ifstream{ entry.path() / "cpufreq/cpuinfo_max_freq" };
        if (auto frequency_in_khz = 0UL; freq_file >> frequency_in_khz) {
          max_frequency_in_hz = std::max(max_frequency_in_hz, frequency_in_khz * 1000UL);
        }
      }
    }
  }

  if (max_frequency_in_hz == 0ULL) {
    throw CannotReadMaxClockFrequency{};
  }

  return HardwareInfo::cache_value(HardwareInfo::_max_cpu_clock_frequency, max_frequency_in_hz);
}

std::optional<std::uint8_t>
perf::HardwareInfo::explore_hardware_counters_experimentally(const bool is_identify_hardware_counters)
{
  /// Translate event names into codes.
  auto events = HardwareInfo::generate_events_for_counter_identification();

  for (auto number_events = static_cast<std::uint8_t>(1U); number_events <= static_cast<std::uint8_t>(events.size());
       ++number_events) {
    auto group = Group{};

    /// Depending on what we want to find, we use either (a) only one event per hardware counter or (b) only one
    /// hardware counter with multiple events.
    auto config = is_identify_hardware_counters ? Config{ /* max groups */ number_events, /* max events */ 1U }
                                                : Config{ /* max groups */ 1U, /* max events */ number_events };

    /// Add the number of events to the group.
    for (auto i = 0U; i < number_events; ++i) {
      group.add(events[i]);
    }

    try {
      /// Try to open the performance counter via the perf subsystem.
      group.open(config);
    } catch (const CannotOpenCounterError& error) {

      /// If the perf subsystem fails with EINVAL, we likely exceeded the limit.
      if (error.error_code() == EINVAL && number_events > 1U) {

        /// When detecting events per counter and the NMI watchdog is enabled, it permanently consumes one hw-PMU
        /// counter. The kernel accepts one event beyond the real limit (open succeeds but all counters read 0),
        /// so we need to subtract 2 instead of 1.
        if (!is_identify_hardware_counters && HardwareInfo::is_nmi_watchdog_enabled() && number_events > 2U) {
          return static_cast<std::uint8_t>(number_events - 2U);
        }

        return static_cast<std::uint8_t>(number_events - 1U);
      }

      return std::nullopt;
    } catch (const std::runtime_error& /*error*/) {
      /// For every other error, we cannot tell the reason.
      return std::nullopt;
    }
  }

  return events.size();
}

std::vector<perf::CounterConfig>
perf::HardwareInfo::generate_events_for_counter_identification()
{
  /// List of event codes.
  auto event_codes = std::vector<CounterConfig>{};
  event_codes.reserve(Group::MAX_MEMBERS);

  /// Fetch all PMU names that are registered.
  const auto pmu_names = CounterDefinition::global().pmu_names();

  /// Check if ARM events are registered. Some ARM CPUs do not support all events provided by the perf subsystem. Hence,
  /// we rely on the events coming from the ARM pmu.
  if (const auto arm_pmu_name =
        std::find_if(pmu_names.begin(),
                     pmu_names.end(),
                     [](const std::string& name) { return name.length() >= 3 && name.substr(0, 3) == "arm"; });
      arm_pmu_name != pmu_names.end()) {
    const auto events = CounterDefinition::global().pmu(*arm_pmu_name);

    /// Translate events into codes.
    for (auto i = 0U; i < std::min<std::size_t>(events.size(), Group::MAX_MEMBERS); ++i) {
      event_codes.push_back(std::get<1>(events[i]));
    }

    return event_codes;
  }

  /// If we could not detect an ARM PMU, we use events provided "cpu" PMU.
  for (const auto& [name, config] : CounterDefinition::global().pmu("cpu")) {

    auto counter = Counter{ config };

    /// Ignore "cycles" and "instructions" events.
    if (config.configs()[0U] == 0U || config.configs()[0U] == 1U) {
      continue;
    }

    /// Try to open the event on a physical performance counter.
    try {
      /// Open as a single (non-live) event on a performance counter.
      counter.open(Config{ 1U, 1U }, false);
    } catch (CannotOpenCounterError&) {
      /// We do not handle the counter as some events will definitely lead to an exception, as not all events provided
      /// by the perf subsystem are supported on any hardware.
      continue;
    }

    /// If the open() call did not throw an exception, we can use the event.
    event_codes.push_back(config);

    /// Check if we reached the limit.
    if (event_codes.size() == Group::MAX_MEMBERS) {
      return event_codes;
    }
  }

  return event_codes;
}