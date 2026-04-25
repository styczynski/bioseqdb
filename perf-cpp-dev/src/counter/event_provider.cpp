#include <algorithm>
#include <fstream>
#include <linux/perf_event.h>
#include <perfcpp/counter/event_provider.hpp>
#include <perfcpp/counter/time_event.hpp>
#include <perfcpp/counter_definition.hpp>
#include <perfcpp/event_file_descriptor_parser.hpp>
#include <perfcpp/exception.hpp>
#include <perfcpp/feature.h>
#include <perfcpp/hardware_info.hpp>
#include <perfcpp/metric/metric.hpp>
#include <regex>
#include <sstream>

void
perf::PerfSubsystemEventProvider::add_events(CounterDefinition& counter_definition)
{
  /// On Intel, instructions, cycles, and ref-cycles use fixed-function PMCs dedicated to these events.
  const auto is_fixed =
    HardwareInfo::is_intel() && HardwareInfo::physical_fixed_performance_counters_per_logical_core() > 0U;
  counter_definition.add("instructions",
                         CounterConfig{ PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, 0UL, 0UL, is_fixed });
  counter_definition.add("cycles", CounterConfig{ PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, 0UL, 0UL, is_fixed });
  counter_definition.add("cpu-cycles",
                         CounterConfig{ PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, 0UL, 0UL, is_fixed });
  counter_definition.add("ref-cycles",
                         CounterConfig{ PERF_TYPE_HARDWARE, PERF_COUNT_HW_REF_CPU_CYCLES, 0UL, 0UL, is_fixed });
  counter_definition.add("bus-cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BUS_CYCLES);

  /// Branches
  counter_definition.add("branches", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS);
  counter_definition.add("branch-instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS);
  counter_definition.add("branch-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES);

  /// Stall events
  counter_definition.add("stalled-cycles-backend", PERF_TYPE_HARDWARE, PERF_COUNT_HW_STALLED_CYCLES_BACKEND);
  counter_definition.add("idle-cycles-backend", PERF_TYPE_HARDWARE, PERF_COUNT_HW_STALLED_CYCLES_BACKEND);
  counter_definition.add("stalled-cycles-frontend", PERF_TYPE_HARDWARE, PERF_COUNT_HW_STALLED_CYCLES_FRONTEND);
  counter_definition.add("idle-cycles-frontend", PERF_TYPE_HARDWARE, PERF_COUNT_HW_STALLED_CYCLES_FRONTEND);

  /// Cache events
  counter_definition.add("cache-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES);
  counter_definition.add("cache-references", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES);
  counter_definition.add("L1-dcache-loads",
                         PERF_TYPE_HW_CACHE,
                         PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                           (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16));
  counter_definition.add("L1-dcache-load-misses",
                         PERF_TYPE_HW_CACHE,
                         PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                           (PERF_COUNT_HW_CACHE_RESULT_MISS << 16));
  counter_definition.add("L1-icache-loads",
                         PERF_TYPE_HW_CACHE,
                         PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                           (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16));
  counter_definition.add("L1-icache-load-misses",
                         PERF_TYPE_HW_CACHE,
                         PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                           (PERF_COUNT_HW_CACHE_RESULT_MISS << 16));

  /// TLB events
  counter_definition.add("dTLB-loads",
                         PERF_TYPE_HW_CACHE,
                         PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                           (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16));
  counter_definition.add("dTLB-load-misses",
                         PERF_TYPE_HW_CACHE,
                         PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                           (PERF_COUNT_HW_CACHE_RESULT_MISS << 16));
  counter_definition.add("iTLB-loads",
                         PERF_TYPE_HW_CACHE,
                         PERF_COUNT_HW_CACHE_ITLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                           (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16));
  counter_definition.add("iTLB-load-misses",
                         PERF_TYPE_HW_CACHE,
                         PERF_COUNT_HW_CACHE_ITLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                           (PERF_COUNT_HW_CACHE_RESULT_MISS << 16));

  /// Software events
  counter_definition.add("software", "cpu-clock", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK);
  counter_definition.add("software", "task-clock", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK);
  counter_definition.add("software", "page-faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS);
  counter_definition.add("software", "faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS);
  counter_definition.add("software", "major-faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS_MAJ);
  counter_definition.add("software", "minor-faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS_MIN);
  counter_definition.add("software", "alignment-faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_ALIGNMENT_FAULTS);
  counter_definition.add("software", "emulation-faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_EMULATION_FAULTS);
  counter_definition.add("software", "context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES);
#ifndef PERFCPP_NO_COUNT_SW_BPF_OUTPUT /// PERF_COUNT_SW_BPF_OUTPUT is supported since Linux Kernel 4.4
  counter_definition.add("software", "bpf-output", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_BPF_OUTPUT);
#endif
#ifndef PERFCPP_NO_CGROUP_SWITCHES /// PERF_COUNT_SW_CGROUP_SWITCHES is supported since Linux Kernel 5.13
  counter_definition.add("software", "cgroup-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CGROUP_SWITCHES);
#endif
  counter_definition.add("software", "cpu-migrations", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS);
  counter_definition.add("software", "migrations", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS);
}

void
perf::TimeEventProvider::add_events(CounterDefinition& counter_definition)
{
  counter_definition.add("seconds", std::make_unique<SecondsTimeEvent>());
  counter_definition.add("s", std::make_unique<SecondsTimeEvent>());
  counter_definition.add("milliseconds", std::make_unique<MillisecondsTimeEvent>());
  counter_definition.add("ms", std::make_unique<MillisecondsTimeEvent>());
  counter_definition.add("microseconds", std::make_unique<MicrosecondsTimeEvent>());
  counter_definition.add("us", std::make_unique<MicrosecondsTimeEvent>());
  counter_definition.add("nanoseconds", std::make_unique<NanosecondsTimeEvent>());
  counter_definition.add("ns", std::make_unique<NanosecondsTimeEvent>());
}

void
perf::MetricEventProvider::add_events(CounterDefinition& counter_definition)
{
  counter_definition.add(std::make_unique<CyclesPerInstruction>());
  counter_definition.add(std::make_unique<Gigahertz>());
  counter_definition.add(std::make_unique<InstructionsPerCycle>());
  counter_definition.add(std::make_unique<CacheHitRatio>());
  counter_definition.add(std::make_unique<CacheMissRatio>());
  counter_definition.add(std::make_unique<DTLBMissRatio>());
  counter_definition.add(std::make_unique<ITLBMissRatio>());
  counter_definition.add(std::make_unique<L1DataMissRatio>());
  counter_definition.add(std::make_unique<BranchMissRatio>());
  counter_definition.add(std::make_unique<WattsPkg>());
  counter_definition.add(std::make_unique<WattsCores>());
  counter_definition.add(std::make_unique<WattsRam>());
}

void
perf::SystemSpecificEventProvider::add_events(CounterDefinition& counter_definition)
{
  auto performance_monitoring_units = std::vector<std::pair<std::string, std::string>>{
    { "/sys/bus/event_source/devices/cpu/", "cpu" },                 /// CPU PMU
    { "/sys/bus/event_source/devices/cpu_core/", "cpu" },            /// CPU PMU on heterogeneous Intel architectures
    { "/sys/bus/event_source/devices/cpu_atom/", "cpu-atom" },       /// Atom PMU on heterogeneous Intel architectures
    { "/sys/bus/event_source/devices/cstate_core/", "cstate-core" }, /// CState Core PMU on Intel architectures
    { "/sys/bus/event_source/devices/cstate_pkg/", "cstate-pkg" },   /// CState Pkg PMU on Intel architectures
    { "/sys/bus/event_source/devices/power/", "power" }              /// Power PMU
  };

  /// Identify more dynamic PMUs (e.g., amd-iommu-0, etc).
  SystemSpecificEventProvider::detect_performance_monitoring_units("amd_iommu.*|armv.*", performance_monitoring_units);

  /// Add the events by reading from the filesystem as specified above.
  for (const auto& [path, pmu_name] : performance_monitoring_units) {
    if (std::filesystem::exists(path)) {
      SystemSpecificEventProvider::add_events(counter_definition, pmu_name, path);
    }
  }
}

void
perf::SystemSpecificEventProvider::add_events(CounterDefinition& counter_definition,
                                              const std::string& pmu_name,
                                              const std::string& path)
{
  auto event_parser = EventFileDescriptorParser{ path };

  /// Parse the type for the PMU.
  if (const auto type = event_parser.type(); type.has_value()) {
    /// Iterate over all files in the descriptor path.
    for (const auto& file_entry : std::filesystem::directory_iterator(path + "events")) {

      /// Events are only described in files without extension.
      if (file_entry.path().extension() == "") {
        auto event_name = file_entry.path().filename().string();

        /// Check if a counter with the given filename already exists. If yes, do not add another.
        if (!counter_definition.counter(pmu_name, event_name).has_value()) {

          /// Parse the file descriptor containing configuration code and further information.
          if (const auto event_configuration = event_parser.config(event_name); event_configuration.has_value()) {

            /// Add the event, if parsing was successfully.
            auto config = CounterConfig{ type.value(),
                                         std::get<0>(event_configuration.value()),
                                         std::get<1>(event_configuration.value()).value_or(0U) };

            /// Try to find and parse a .scale file for the given event. Only a few events (e.g., the power PMU)
            /// provide/need a scale factor.
            if (const auto scale = event_parser.scale(event_name); scale.has_value()) {
              config.scale(scale.value());
            }

            counter_definition.add(std::string{ pmu_name }, std::move(event_name), config);
          }
        }
      }
    }
  }
}

void
perf::SystemSpecificEventProvider::detect_performance_monitoring_units(
  std::string&& regex_pattern,
  std::vector<std::pair<std::string, std::string>>& performance_monitoring_units)
{
  const auto pattern = std::regex{ regex_pattern };

  /// Iterate through the directory
  for (const auto& entry : std::filesystem::directory_iterator("/sys/bus/event_source/devices/")) {
    if (entry.is_directory()) {
      /// Check if folder name matches the regex pattern
      if (const auto subfolder = entry.path().filename().string(); std::regex_match(subfolder, pattern)) {

        /// If yes, replace all underscores by dashes.
        auto pmu_name = subfolder;
        std::replace(pmu_name.begin(), pmu_name.end(), '_', '-');

        performance_monitoring_units.emplace_back(entry.path().string().append("/"), std::move(pmu_name));
      }
    }
  }
}

void
perf::AMDIbsEventProvider::add_events(CounterDefinition& counter_definition)
{
  /// AMD's Instruction Based Sampling differs in configuration (and utilization) from Intel PEBS with specific PMUs for
  /// sampling. Whenever an AMD CPU is detected, IBS PMUs will be added.
  if (HardwareInfo::is_amd_ibs_supported()) {
    AMDIbsEventProvider::add_fetch_events(counter_definition);
    AMDIbsEventProvider::add_op_events(counter_definition);
  }
}

void
perf::AMDIbsEventProvider::add_fetch_events(CounterDefinition& counter_definition)
{
  const auto& ibs_info = HardwareInfo::amd_ibs();
  if (const auto fetch_type = ibs_info.fetch_type(); ibs_info.is_supported() && fetch_type.has_value()) {
    /// Setup random bit if requested.
    auto rand_value = 0UL;
    if (const auto rand_bit = ibs_info.fetch_rand_bit(); rand_bit.has_value()) {
      rand_value = 1UL << rand_bit.value();
    }

    /// Event that is triggered by cycles.
    counter_definition.add("ibs_fetch", "ibs_fetch", CounterConfig{ fetch_type.value(), rand_value });

    if (const auto l3_miss_only_bit = ibs_info.fetch_l3_miss_only_bit(); l3_miss_only_bit.has_value()) {
      /// Event that is triggered by cycles and applies the L3 miss filter.
      counter_definition.add("ibs_fetch",
                             "ibs_fetch_l3missonly",
                             CounterConfig{ fetch_type.value(), rand_value | (1ULL << l3_miss_only_bit.value()) });
    }
  }
}

void
perf::AMDIbsEventProvider::add_op_events(perf::CounterDefinition& counter_definition)
{
  const auto& ibs_info = HardwareInfo::amd_ibs();
  if (const auto op_type = ibs_info.op_type(); ibs_info.is_supported() && op_type.has_value()) {
    /// Event that is triggered by cycles.
    counter_definition.add("ibs_op", "ibs_op", CounterConfig{ op_type.value(), 0U });

    if (const auto uops_bit = ibs_info.op_uops_bit(); uops_bit.has_value()) {
      /// Event that is triggered by micro ops.
      counter_definition.add("ibs_op", "ibs_op_uops", CounterConfig{ op_type.value(), 1ULL << uops_bit.value() });
    }

    if (const auto l3_miss_only_bit = ibs_info.op_l3_miss_only_bit(); l3_miss_only_bit.has_value()) {
      /// Event that is triggered by cycles and applies the L3 miss only filter.
      counter_definition.add(
        "ibs_op", "ibs_op_l3missonly", CounterConfig{ op_type.value(), 1ULL << l3_miss_only_bit.value() });

      /// Event that is triggered by uops and applies the L3 miss only filter.
      if (const auto uops_bit = ibs_info.op_uops_bit(); uops_bit.has_value()) {
        counter_definition.add(
          "ibs_op",
          "ibs_op_uops_l3missonly",
          CounterConfig{ op_type.value(), (1ULL << uops_bit.value()) | (1ULL << l3_miss_only_bit.value()) });
      }
    }
  }
}

void
perf::CsvFileEventProvider::add_events(perf::CounterDefinition& counter_definition)
{
  /// Read all counter values from the config file in the format
  ///     name,<config>[,<extended config>,<type>]
  /// where <config> and <extended config> are either integer or hex values.

  auto input_file = std::ifstream{ this->_file_name };
  if (!input_file.is_open()) {
    throw CannotOpenFileError{ this->_file_name };
  }

  std::string line;
  while (std::getline(input_file, line)) {
    /// Skip lines that start with '#' and are considered a comment.
    if (const auto is_comment = !line.empty() && line.front() == '#'; is_comment) {
      continue;
    }

    /// Lines containing a ',' (separator) are considered an event or an event.
    if (const auto is_event_line = line.find(',') != std::string::npos; is_event_line) {
      auto line_stream = std::istringstream{ line };

      std::string name;

      /// Read name.
      if (std::getline(line_stream, name, ','); !name.empty()) {

        auto extended_config = std::optional<std::uint64_t>{ std::nullopt };
        auto type = std::optional<std::uint32_t>{ std::nullopt };

        /// Read config-field and translate into integer.
        if (std::string config_or_metric_str; std::getline(line_stream, config_or_metric_str, ',')) {

          /// Try to translate config into number.
          if (const auto config = EventFileDescriptorParser::integer(config_or_metric_str); config.has_value()) {
            /// Read extended config-field and translate into integer.
            if (std::string extended_config_str; std::getline(line_stream, extended_config_str, ',')) {
              /// Translate extended config into number.
              extended_config = EventFileDescriptorParser::integer(extended_config_str);

              /// Read type-field and translate into integer.
              if (std::string type_str; std::getline(line_stream, type_str, ',')) {
                /// Translate type into number.
                type = EventFileDescriptorParser::integer(type_str);
              }
            }

            /// Add counter configuration.
            counter_definition.add(
              std::move(name),
              CounterConfig{ type.value_or(PERF_TYPE_RAW), config.value(), extended_config.value_or(0ULL) });
          }

          /// Try to translate config into metric.
          else if (!config_or_metric_str.empty()) {
            auto metric = std::make_unique<FormulaMetric>(std::move(name), std::move(config_or_metric_str));
            counter_definition.add(std::move(metric));
          }
        }
      }
    }
  }
}
