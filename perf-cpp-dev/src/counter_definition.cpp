#include <fstream>
#include <perfcpp/counter_definition.hpp>
#include <perfcpp/hardware_info.hpp>
#include <perfcpp/util/table.hpp>
#include <sstream>

/// The global CounterDefinition instance is used as a default one for EventCounter and Sampler. This instance detects
/// counters from the perf subsystem and (if activated) processor-specific events. Further (child) counter definitions
/// will inherit the registered events, metrics, and time events.
std::shared_ptr<perf::CounterDefinition> perf::CounterDefinition::_global = CounterDefinition::make_global();

perf::CounterDefinition::CounterDefinition(std::unique_ptr<EventProvider>&& event_provider)
  : _parent_counter_definition(CounterDefinition::_global)
{
  /// Reserve space for events.
  this->_performance_monitoring_unit_events.reserve(8U);
  this->_metrics.reserve(32U);
  this->_time_events.reserve(8U);

  /// Additional event provider, if specified. For example, this could be a provider adding events from a file.
  if (event_provider != nullptr) {
    event_provider->add_events(*this);
  }
}

perf::CounterDefinition::CounterDefinition(const std::string& config_file)
  : CounterDefinition(std::make_unique<CsvFileEventProvider>(config_file))
{
}

std::shared_ptr<perf::CounterDefinition>
perf::CounterDefinition::make_global()
{
  /// Create the global counter definition.
  auto global_counter_definition = std::make_shared<CounterDefinition>();

  auto event_providers = std::vector<std::unique_ptr<EventProvider>>{};
  event_providers.reserve(8U);

  /// Collect all generic event providers.
  event_providers.push_back(std::make_unique<PerfSubsystemEventProvider>());
  event_providers.push_back(std::make_unique<SystemSpecificEventProvider>());
  event_providers.push_back(std::make_unique<MetricEventProvider>());
  event_providers.push_back(std::make_unique<TimeEventProvider>());

#ifdef PERFCPP_HAS_PROCESSOR_SPECIFIC_EVENTS
  /// Hardware-specific events.
  event_providers.push_back(std::make_unique<ProcessorSpecificEventProvider>());
#endif

  /// AMD-specific event provider.
  if (HardwareInfo::is_amd()) {
    event_providers.push_back(std::make_unique<AMDIbsEventProvider>());
  }

  /// Let the event providers add events.
  for (const auto& provider : event_providers) {
    provider->add_events(*global_counter_definition);
  }

  return global_counter_definition;
}

void
perf::CounterDefinition::add(std::string&& pmu_name, std::string&& event_name, const CounterConfig config)
{
  /// When the PMU (identified by the name) already exists, add the event.
  if (const auto pmu_iterator = this->_performance_monitoring_unit_events.find(pmu_name);
      pmu_iterator != this->_performance_monitoring_unit_events.end()) {
    auto& event_config_map = pmu_iterator->second;
    event_config_map.insert_or_assign(std::move(event_name), config);
  }

  /// Otherwise reserve some space for upcoming events for that PMU and add that map.
  else {
    auto event_config_map = std::unordered_map<std::string, perf::CounterConfig>{};
    event_config_map.reserve(64U);

    event_config_map.insert(std::make_pair(std::move(event_name), config));

    this->_performance_monitoring_unit_events.insert(std::make_pair(std::move(pmu_name), std::move(event_config_map)));
  }
}

std::vector<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::CounterDefinition::counter(const std::string& name) const noexcept
{
  auto event_configurations = std::vector<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>{};
  event_configurations.reserve(this->_performance_monitoring_unit_events.size());

  /// Scan all PMUs...
  for (const auto& [pmu_name, events] : this->_performance_monitoring_unit_events) {
    /// ... for an event with that requested name.
    if (auto iterator = events.find(name); iterator != events.end()) {
      event_configurations.emplace_back(
        std::string_view(pmu_name), std::string_view(iterator->first), iterator->second);
    }
  }

  /// Scan all PMUs of parent counter definition.
  if (this->_parent_counter_definition != nullptr) {
    if (auto parent_event_configurations = this->_parent_counter_definition->counter(name);
        !parent_event_configurations.empty()) {
      std::move(parent_event_configurations.begin(),
                parent_event_configurations.end(),
                std::back_inserter(event_configurations));
    }
  }

  return event_configurations;
}

std::optional<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::CounterDefinition::counter(const std::string& pmu_name, const std::string& event_name) const noexcept
{
  /// Find all events of the PMU.
  if (const auto pmu_iterator = this->_performance_monitoring_unit_events.find(pmu_name);
      pmu_iterator != this->_performance_monitoring_unit_events.end()) {
    const auto& pmu_events = pmu_iterator->second;

    /// Find the event in the PMU event list.
    if (const auto event_iterator = pmu_events.find(event_name); event_iterator != pmu_events.end()) {
      return std::make_optional(std::make_tuple(
        std::string_view{ pmu_iterator->first }, std::string_view{ event_iterator->first }, event_iterator->second));
    }
  }

  /// If the counter wasn't found, try to find it in the parent's list.
  if (this->_parent_counter_definition != nullptr) {
    return this->_parent_counter_definition->counter(pmu_name, event_name);
  }

  return std::nullopt;
}

std::optional<std::pair<std::string_view, perf::Metric&>>
perf::CounterDefinition::metric(const std::string& name) const noexcept
{
  if (const auto iterator = this->_metrics.find(name); iterator != this->_metrics.end()) {
    return std::make_optional(std::make_pair(std::string_view(iterator->first), std::ref(*iterator->second)));
  }

  /// If the metric wasn't found, try to find it in the parent's list.
  if (this->_parent_counter_definition != nullptr) {
    return this->_parent_counter_definition->metric(name);
  }

  return std::nullopt;
}

std::optional<std::pair<std::string_view, perf::TimeEvent&>>
perf::CounterDefinition::time_event(const std::string& name) const noexcept
{
  if (const auto iterator = this->_time_events.find(name); iterator != this->_time_events.end()) {
    return std::make_optional(std::make_pair(std::string_view(iterator->first), std::ref(*iterator->second)));
  }

  /// If the time event wasn't found, try to find it in the parent's list.
  if (this->_parent_counter_definition != nullptr) {
    return this->_parent_counter_definition->time_event(name);
  }

  return std::nullopt;
}

std::vector<std::pair<std::string_view, perf::CounterConfig>>
perf::CounterDefinition::pmu(const std::string& pmu_name) const
{
  auto events = std::vector<std::pair<std::string_view, CounterConfig>>{};

  if (const auto iterator = this->_performance_monitoring_unit_events.find(pmu_name);
      iterator != this->_performance_monitoring_unit_events.end()) {
    std::transform(iterator->second.begin(), iterator->second.end(), std::back_inserter(events), [](const auto& pair) {
      return std::make_pair(std::string_view{ std::get<0>(pair) }, std::get<1>(pair));
    });
  }

  /// Append parent PMU names, if there is a parent.
  if (this->_parent_counter_definition != nullptr) {
    if (auto parent_counters = this->_parent_counter_definition->pmu(pmu_name); !parent_counters.empty()) {
      std::move(parent_counters.begin(), parent_counters.end(), std::back_inserter(events));
    }
  }

  return events;
}

bool
perf::CounterDefinition::is_metric(const std::string& name) const noexcept
{
  /// Check if the metric is registered in this instance.
  if (this->_metrics.find(name) != this->_metrics.end()) {
    return true;
  }

  /// Check if the metric is registered in the parent's instance.
  if (this->_parent_counter_definition != nullptr) {
    return this->_parent_counter_definition->is_metric(name);
  }

  return false;
}

bool
perf::CounterDefinition::is_time_event(const std::string& name) const noexcept
{
  /// Check if the time event is registered in this instance.
  if (this->_time_events.find(name) != this->_time_events.end()) {
    return true;
  }

  /// Check if the time event is registered in the parent's instance.
  if (this->_parent_counter_definition != nullptr) {
    return this->_parent_counter_definition->is_time_event(name);
  }

  return false;
}

std::vector<std::string>
perf::CounterDefinition::pmu_names() const
{
  auto names = std::vector<std::string>{};

  /// Map the events hash table to PMU names (i.e., the key).
  std::transform(this->_performance_monitoring_unit_events.begin(),
                 this->_performance_monitoring_unit_events.end(),
                 std::back_inserter(names),
                 [](const auto& config) { return config.first; });

  /// Append parent PMU names, if there is a parent.
  if (this->_parent_counter_definition != nullptr) {
    if (auto parent_pmu_names = this->_parent_counter_definition->pmu_names(); !parent_pmu_names.empty()) {
      std::move(parent_pmu_names.begin(), parent_pmu_names.end(), std::back_inserter(names));
    }
  }

  return names;
}

std::vector<std::string>
perf::CounterDefinition::metric_names() const
{
  auto names = std::vector<std::string>{};

  /// Map metrics to names.
  std::transform(this->_metrics.begin(), this->_metrics.end(), std::back_inserter(names), [](const auto& config) {
    return config.first;
  });

  /// Append parent metric names, if there is a parent.
  if (this->_parent_counter_definition != nullptr) {
    if (auto parent_metric_names = this->_parent_counter_definition->metric_names(); !parent_metric_names.empty()) {
      std::move(parent_metric_names.begin(), parent_metric_names.end(), std::back_inserter(names));
    }
  }

  return names;
}

std::vector<std::string>
perf::CounterDefinition::time_event_names() const
{
  auto names = std::vector<std::string>{};

  /// Map time events to names.
  std::transform(this->_time_events.begin(),
                 this->_time_events.end(),
                 std::back_inserter(names),
                 [](const auto& config) { return config.first; });

  /// Append parent time names, if there is a parent.
  if (this->_parent_counter_definition != nullptr) {
    if (auto parent_time_event_names = this->_parent_counter_definition->time_event_names();
        !parent_time_event_names.empty()) {
      std::move(parent_time_event_names.begin(), parent_time_event_names.end(), std::back_inserter(names));
    }
  }

  return names;
}

bool
perf::CounterDefinition::supports(const std::string_view name) const
{
  if (!this->counter(name).empty()) {
    return true;
  }

  if (this->time_event(name).has_value()) {
    return true;
  }

  if (const auto metric = this->metric(name); metric.has_value()) {
    /// Check all events required by the metric.
    const auto required_counter_names = std::get<1>(metric.value()).required_counter_names();
    return std::all_of(required_counter_names.begin(), required_counter_names.end(), [this](const auto& counter_name) {
      return this->supports(counter_name);
    });
  }

  return false;
}

std::string
perf::CounterDefinition::to_string() const
{
  /// Lambda to turn decimal value into a hexadecimal string. Used to print event ids.
  const auto decimal_to_hex_string = [](const auto decimal) -> std::string {
    auto stream = std::stringstream{};
    stream << "0x" << std::hex << decimal << std::dec;
    return stream.str();
  };

  /// Lambda to turn a decimal value into a scientific value.
  const auto double_to_scientific = [](const auto decimal) -> std::string {
    if (decimal == 1.) {
      return "1";
    }
    auto stream = std::stringstream{};
    stream << std::scientific << decimal << std::dec;
    return stream.str();
  };

  auto table = util::Table{};

  // Header.
  table.add({ util::Table::Header{ "PMU", util::Table::Alignment::Left },
              util::Table::Header{ "name", util::Table::Alignment::Left },
              util::Table::Header{ "type", util::Table::Alignment::Left },
              util::Table::Header{ "config", util::Table::Alignment::Left },
              util::Table::Header{ "config1", util::Table::Alignment::Left },
              util::Table::Header{ "config2", util::Table::Alignment::Left },
              util::Table::Header{ "scale", util::Table::Alignment::Left } });

  /// Read all PMUs from all (parent) definitions.
  auto pmu_names = this->pmu_names();
  std::sort(pmu_names.begin(), pmu_names.end());

  /// Print all events from all PMUs.
  for (const auto& pmu_name : pmu_names) {
    auto events = this->pmu(pmu_name);
    std::sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
      return std::get<0>(left) < std::get<0>(right);
    });

    for (const auto& [name, config] : events) {
      auto row = util::Table::Row{};

      row << pmu_name << std::string{ name } << config.type() << decimal_to_hex_string(config.configs()[0U])
          << decimal_to_hex_string(config.configs()[1U]) << decimal_to_hex_string(config.configs()[2U])
          << double_to_scientific(config.scale());
      table.add(std::move(row));
    }
  }

  /// Add all metrics to the table.
  auto metric_names = this->metric_names();
  std::sort(metric_names.begin(), metric_names.end());
  for (const auto& name : metric_names) {
    auto row = util::Table::Row{};
    row << "metric" << name << "" << "" << "" << "" << "";
    table.add(std::move(row));
  }

  /// Add all virtual time events to the table.
  auto time_event_names = this->time_event_names();
  std::sort(time_event_names.begin(), time_event_names.end());
  for (const auto& name : time_event_names) {
    auto row = util::Table::Row{};
    row << "time" << name << "" << "" << "" << "" << "";
    table.add(std::move(row));
  }

  return table.to_string();
}