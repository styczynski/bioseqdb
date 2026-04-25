#include <algorithm>
#include <numeric>
#include <perfcpp/event_counter.hpp>

#include "perfcpp/hardware_info.hpp"

#include <perfcpp/exception.hpp>
#include <utility>

perf::EventCounter
perf::EventCounter::copy_from_template(const EventCounter& other)
{
  auto copy = EventCounter{
    other._counter_definition, other._config, other._requested_event_set, other._requested_live_event_set
  };

  copy._num_fixed_groups = other._num_fixed_groups;

  copy._hardware_event_groups.reserve(other._hardware_event_groups.size());
  for (const auto& [group, is_open] : other._hardware_event_groups) {
    copy._hardware_event_groups.emplace_back(Group::copy_from_template(group), is_open);
  }

  copy._hardware_live_counters.reserve(other._hardware_live_counters.size());
  for (const auto& live_counter : other._hardware_live_counters) {
    copy._hardware_live_counters.push_back(Counter::copy_from_template(live_counter));
  }

  return copy;
}

perf::EventCounter::~EventCounter() noexcept(false)
{
  this->close();
}

void
perf::EventCounter::add(const std::string& event_name, const Schedule schedule)
{
  if (this->_is_opened) {
    throw CannotAddEventWhenOpenedError{};
  }

  auto events = std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>{};
  this->expand_to_events(event_name, true, events);

  /// Schedule the events to hardware counters.
  this->schedule(std::move(events), schedule);
}

void
perf::EventCounter::add(const std::vector<std::string>& event_names, const Schedule schedule)
{
  if (this->_is_opened) {
    throw CannotAddEventWhenOpenedError{};
  }

  auto events = std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>{};
  events.reserve(8U);

  /// Unfold all events.
  for (const auto& event_name : event_names) {
    this->expand_to_events(event_name, true, events);
  }

  /// Schedule the events to hardware counters.
  this->schedule(std::move(events), schedule);
}

void
perf::EventCounter::expand_to_events(const std::string& name,
                                     const bool is_visible_in_results,
                                     std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>& events) const
{
  /// If the event name contains a '/', it might be in the format "pmu/event". Probe the event list for this patter.
  /// However, if we cannot detect a (pmu, event) pair, it might be metric or just an event containing a slash.
  if (const auto pmu_delimiter_pos = name.find('/'); pmu_delimiter_pos != std::string::npos) {
    const auto pmu_name = name.substr(0U, pmu_delimiter_pos);
    const auto name_without_pmu = name.substr(pmu_delimiter_pos + 1U, name.length() - (pmu_delimiter_pos + 1U));

    if (const auto event_configurations = this->_counter_definition.counter(pmu_name, name_without_pmu);
        event_configurations.has_value()) {
      auto [global_pmu_name, global_event_name, config] = event_configurations.value();
      EventCounter::add(global_pmu_name,
                        global_event_name,
                        config,
                        /* requested hardware events are visible */ is_visible_in_results,
                        events);
      return;
    }
  }

  /// Find all events with the requested name.
  if (const auto event_configurations = this->_counter_definition.counter(name); !event_configurations.empty()) {
    for (const auto& [pmu_name, event_name, config] : event_configurations) {
      EventCounter::add(pmu_name,
                        event_name,
                        config,
                        /* requested hardware events are visible */ is_visible_in_results,
                        events);
    }
  }

  /// If the given name references an existing metric, add the metric and all its required counters.
  else if (const auto metric = this->_counter_definition.metric(name); metric.has_value()) {
    const auto& [metric_name, metric_instance] = metric.value();

    /// Add all hardware counters required by the metric..
    for (auto& dependent_event_name : metric_instance.required_counter_names()) {

      /// Check if the dependent event is already in the list.
      if (std::find_if(events.begin(), events.end(), [&dependent_event_name](const auto& requested_event) {
            return std::get<0>(requested_event).event_name() == dependent_event_name;
          }) == events.end()) {
        /// Unfold dependent events recursively.
        this->expand_to_events(dependent_event_name, false, events);
      }
    }

    /// If all of the metric's events could be added (i.e., no exception was thrown), add the metric itself.
    events.emplace_back(RequestedEvent{ metric_name, is_visible_in_results, RequestedEvent::Type::Metric },
                        std::nullopt);
  }

  /// If the given name references an existing time event, add the time event.
  else if (const auto& time_event = this->_counter_definition.time_event(name); time_event.has_value()) {
    events.emplace_back(
      RequestedEvent{ std::get<0>(time_event.value()), is_visible_in_results, RequestedEvent::Type::TimeEvent },
      std::nullopt);
  } else {
    throw CannotFindEventOrMetricError{ name };
  }
}

void
perf::EventCounter::add(const std::string_view pmu_name,
                        const std::string_view event_name,
                        const CounterConfig& event_config,
                        const bool is_shown_in_results,
                        std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>& events)
{
  /// Find an event with the same name in the result vector.
  auto iterator = std::find_if(events.begin(), events.end(), [pmu_name, event_name](const auto& requested_event) {
    const auto& event = std::get<0>(requested_event);
    return (!event.pmu_name().has_value() || event.pmu_name().value() == pmu_name) && event.event_name() == event_name;
  });
  if (iterator == events.end()) {
    /// Append, if the event does not exist.
    events.emplace_back(
      RequestedEvent{ pmu_name, event_name, is_shown_in_results, RequestedEvent::Type::HardwareEvent }, event_config);
  } else if (is_shown_in_results) {
    /// Adjust visibility if the event should be shown.
    iterator->first.shown_in_results(true);
  }
}

void
perf::EventCounter::schedule(std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>&& events,
                             const Schedule schedule)
{
  /// On Intel, fixed-function PMCs hold dedicated events (instructions, cycles, ref-cycles).
  /// Each fixed event gets its own pinned group and does not count against the generic PMC limit.
  if (HardwareInfo::is_intel() && HardwareInfo::physical_fixed_performance_counters_per_logical_core() > 0U) {
    events = this->schedule_to_fixed_hardware_counters(std::move(events));
    if (events.empty()) {
      return;
    }
  }

  if (schedule == Schedule::Append || schedule == Schedule::Separate) {
    this->schedule_to_generic_hardware_counters(std::move(events), schedule);
  } else if (schedule == Schedule::Group) {
    this->schedule_as_group(std::move(events));
  }
}

std::vector<std::pair<perf::RequestedEvent, std::optional<perf::CounterConfig>>>
perf::EventCounter::schedule_to_fixed_hardware_counters(
  std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>&& events)
{
  for (auto& [requested_event, config] : events) {
    if (!config.has_value() || !config->is_fixed()) {
      continue;
    }

    /// Skip if the event is already scheduled (e.g., required by multiple metrics).
    if (this->_requested_event_set.adjust_visibility_if_present(
          requested_event.pmu_name(), requested_event.event_name(), requested_event.is_shown_in_results())) {
      continue;
    }

    /// Create a dedicated group for this fixed event; fixed groups do not count against the generic PMC limit.
    this->create_new_group(requested_event, config.value(), /* fixed events never share a group */ false);
    ++this->_num_fixed_groups;
  }

  /// Remove fixed events that have been scheduled above.
  events.erase(std::remove_if(events.begin(),
                              events.end(),
                              [](const auto& event_and_config) {
                                return std::get<1>(event_and_config).has_value() &&
                                       std::get<1>(event_and_config)->is_fixed();
                              }),
               events.end());

  return events;
}

void
perf::EventCounter::schedule_to_generic_hardware_counters(
  std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>&& events,
  const Schedule schedule)
{
  for (auto& [requested_event, event_configuration] : events) {
    /// Metrics and time events (indicated by no hardware event config) do not need to be scheduled to hardware
    /// counter groups; just add them to the event set.
    if (!event_configuration.has_value()) {
      this->_requested_event_set.add(requested_event);
      continue;
    }

    /// If the event is already in the set, only adjust its visibility; do not add the event twice.
    if (this->_requested_event_set.adjust_visibility_if_present(
          requested_event.pmu_name(), requested_event.event_name(), requested_event.is_shown_in_results())) {
      continue;
    }

    /// When appending, try to find an open group and schedule the event there.
    if (schedule == Schedule::Append &&
        this->append_to_any_hardware_counter(requested_event, event_configuration.value())) {
      continue;
    }

    /// No open group found (or Separate mode): create a new group. Append keeps the group open for future events;
    /// Separate closes it immediately.
    this->create_new_group(requested_event,
                           event_configuration.value(),
                           /* keep open for append mode */ schedule == Schedule::Append);
  }
}

void
perf::EventCounter::schedule_as_group(std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>&& events)
{
  /// Test if we can add another group.
  if (this->size() == this->_config.num_physical_counters()) {
    throw MaxPhysicalCountersReachedError{ this->_config.num_physical_counters() };
  }

  /// Test if all hardware events fit into a single group.
  const auto count_hardware_events = std::count_if(
    events.begin(), events.end(), [](const auto& requested_event) { return std::get<1>(requested_event).has_value(); });
  if (count_hardware_events > this->_config.num_events_per_physical_counter()) {
    throw CannotAddEventToSingleGroupError{ this->_config.num_events_per_physical_counter() };
  }

  /// Check for every event if it should be added to a possible group. We keep the events that will be added.
  auto events_to_add = std::vector<std::pair<RequestedEvent, CounterConfig>>{};
  events_to_add.reserve(events.size());

  for (auto& [requested_event, event_configuration] : events) {
    /// Metrics and time events (indicated by no hardware event config) do not need to be scheduled to hardware
    /// counter groups; just add them to the event set.
    if (!event_configuration.has_value()) {
      this->_requested_event_set.add(requested_event);
      continue;
    }

    /// If the event is already in the set, only adjust its visibility; do not add the event twice.
    if (this->_requested_event_set.adjust_visibility_if_present(
          requested_event.pmu_name(), requested_event.event_name(), requested_event.is_shown_in_results())) {
      continue;
    }

    /// Remember the index – this event will be added to the group.
    events_to_add.emplace_back(requested_event, event_configuration.value());
  }

  /// If at least one event survived, we create a new group.
  if (!events_to_add.empty()) {
    auto& [group, _] = this->_hardware_event_groups.emplace_back(Group{}, false);
    const auto group_id = static_cast<std::uint8_t>(this->_hardware_event_groups.size() - 1U);

    for (auto& [requested_event, event_configuration] : events_to_add) {
      const auto in_group_position = static_cast<std::uint8_t>(group.size());
      group.add(event_configuration);
      this->_requested_event_set.add(requested_event, group_id, in_group_position);
    }
  }
}

bool
perf::EventCounter::append_to_any_hardware_counter(RequestedEvent& event, const CounterConfig& event_config)
{
  for (auto group_id = 0U; group_id < this->_hardware_event_groups.size(); ++group_id) {
    if (const auto is_group_open = std::get<1>(this->_hardware_event_groups[group_id]); is_group_open) {
      /// We found a matching group that has space.
      auto& [group, is_keep_open] = this->_hardware_event_groups[group_id];
      const auto in_group_position = static_cast<std::uint8_t>(group.size());

      /// Add to the hardware counter group.
      group.add(event_config);

      /// Add to the request set.
      this->_requested_event_set.add(event, static_cast<std::uint8_t>(group_id), in_group_position);

      /// Close the group if full.
      if (group.size() == this->_config.num_events_per_physical_counter()) {
        is_keep_open = false;
      }

      return true;
    }
  }

  return false;
}

void
perf::EventCounter::create_new_group(RequestedEvent& event, const CounterConfig& event_config, bool is_keep_open)
{
  /// Test if we can add another group.
  if (this->size() == this->_config.num_physical_counters()) {
    throw MaxPhysicalCountersReachedError{ this->_config.num_physical_counters() };
  }

  /// Only keep the group open if more than one event is allowed per group.
  is_keep_open &= this->_config.num_events_per_physical_counter() > 1U;

  /// Create a new group and add the event, if we did not raise an exception.
  auto& [group, _] = this->_hardware_event_groups.emplace_back(Group{}, is_keep_open);
  group.add(event_config);

  /// Add to the request set.
  const auto group_id = static_cast<std::uint8_t>(this->_hardware_event_groups.size() - 1U);
  this->_requested_event_set.add(event, group_id, /* the event is the first in the group */ 0U);
}

void
perf::EventCounter::add_live(const std::string& event_name)
{
  if (this->_is_opened) {
    throw CannotAddEventWhenOpenedError{};
  }

  /// If the given name references one or multiple existing counters, add it.
  if (auto event_configurations = this->_counter_definition.counter(event_name); !event_configurations.empty()) {
    for (auto [pmu_name, name, event_configuration] : event_configurations) {
      if (this->size() == this->_config.num_physical_counters()) {
        throw MaxCountersReachedError{ this->_config.num_physical_counters() };
      }

      this->_hardware_live_counters.emplace_back(event_configuration);

      /// Add the event to the requested event set. Since every live event is scheduled to a dedicated physical hardware
      /// counter, every event will be the first in the group.
      this->_requested_live_event_set.add(
        RequestedEvent{ pmu_name,
                        name,
                        static_cast<std::uint8_t>(this->_hardware_live_counters.size() - 1U),
                        /* position in group */ 0U });
    }

    return;
  }

  /// If the event does not exist, check if it is a metric, which is not supported for live events. Let the user know.
  if (this->_counter_definition.is_metric(event_name)) {
    throw MetricNotSupportedAsLiveEventError{ event_name };
  }

  /// If the event does not exist, check if it is a time event, which is not supported for live events. Let the user
  /// know.
  if (this->_counter_definition.is_time_event(event_name)) {
    throw TimeEventNotSupportedAsLiveEventError{ event_name };
  }

  throw CannotFindEventError{ event_name };
}

void
perf::EventCounter::add_live(std::vector<std::string>&& event_names)
{
  for (const auto& event_name : event_names) {
    this->add_live(event_name);
  }
}

void
perf::EventCounter::open()
{
  /// Measuring any CPU core and any process is invalid, according to the perf subsystem documentation.
  if (this->_config.cpu_core().is_any() && this->_config.process().is_any()) {
    throw InvalidConfigAnyCpuCoreAndAnyProcess{};
  }

  /// Verify that the EventCounter is not already opened (_is_open == false) and set flag appropriately.
  if (const auto was_open = std::exchange(this->_is_opened, true); !was_open) {
    /// Open all groups. If one of them fails, group.open() will throw an exception.
    for (auto& [group, _] : this->_hardware_event_groups) {
      group.open(this->_config);
    }

    /// Open all live counters. If one of them fails, counter.open() will throw an exception.
    for (auto& live_counter : this->_hardware_live_counters) {
      live_counter.open(this->_config, /* is live counter */ true);
    }
  }
}

void
perf::EventCounter::start()
{
  /// Opens the hardware performance counters, if not already done specifically by calling EventCounter::open().
  this->open();

  /// Start all groups. If one of them fails, group.start() will throw an exception.
  for (auto& [group, _] : this->_hardware_event_groups) {
    group.start();
  }

  /// Start all live counters.
  for (auto& live_counter : this->_hardware_live_counters) {
    live_counter.enable();
  }

  /// Start timer.
  std::get<0>(this->_start_and_end_time) = std::chrono::steady_clock::now();
}

void
perf::EventCounter::stop()
{
  /// Stop timer.
  std::get<1>(this->_start_and_end_time) = std::chrono::steady_clock::now();

  /// Stop all groups.
  for (auto& [group, _] : this->_hardware_event_groups) {
    group.stop();
  }

  /// Stop all live counters.
  for (auto& live_counter : this->_hardware_live_counters) {
    live_counter.disable();
  }
}

void
perf::EventCounter::close()
{
  if (const auto is_open = std::exchange(this->_is_opened, false); is_open) {
    /// Close all groups.
    for (auto& [group, _] : this->_hardware_event_groups) {
      group.close();
    }

    /// Close all live counters.
    for (auto& live_counter : this->_hardware_live_counters) {
      live_counter.close();
    }
  }
}

perf::CounterResult
perf::EventCounter::result(const std::uint64_t normalization) const
{
  /// Build result with all events, including hidden ones.
  auto event_values = std::vector<std::pair<std::string_view, double>>{};
  event_values.reserve(this->_requested_event_set.size());

  /// Copy only the hardware- and time-event values.
  for (const auto& event : this->_requested_event_set) {
    /// Hardware events are read from the hardware counter (groups).
    if (event.is_hardware_event()) {
      if (auto scheduled_group = event.scheduled_group(); scheduled_group.has_value()) {
        const auto& group = std::get<0>(this->_hardware_event_groups[scheduled_group->id()]);
        event_values.emplace_back(event.event_name(), group.get(scheduled_group->position()));
      }
    }
    /// Time events are read using the event's start and stop time.
    else if (event.is_time_event()) {
      if (const auto& time_calculator = this->_counter_definition.time_event(event.event_name());
          time_calculator.has_value()) {
        const auto [start_timestamp, stop_timestamp] = this->_start_and_end_time;
        const auto time = std::get<1>(time_calculator.value()).calculate(start_timestamp, stop_timestamp);
        event_values.emplace_back(event.event_name(), time);
      }
    }
  }

  /// Turn the result of only hardware events into a result containing requested hardware events and metrics (which are
  /// calculated from hardware events).
  return this->_requested_event_set.result(
    this->_counter_definition, CounterResult{ std::move(event_values) }, normalization);
}

void
perf::EventCounter::live_result(std::vector<double>& result) const
{
  if (this->_hardware_live_counters.size() != result.size()) {
    throw LiveEventCounterResultMismatchError{ this->_hardware_live_counters.size(), result.size() };
  }

  for (auto counter_id = 0U; counter_id < this->_hardware_live_counters.size(); ++counter_id) {
    result[counter_id] = this->live_result(counter_id).value_or(.0);
  }
}

void
perf::EventCounter::live_result(std::vector<double>& result, std::uint64_t normalization) const
{
  if (this->_hardware_live_counters.size() != result.size()) {
    throw LiveEventCounterResultMismatchError{ this->_hardware_live_counters.size(), result.size() };
  }

  for (auto counter_id = 0U; counter_id < this->_hardware_live_counters.size(); ++counter_id) {
    result[counter_id] = this->live_result(counter_id, normalization).value_or(.0);
  }
}

std::optional<double>
perf::EventCounter::live_result(const std::uint64_t counter_index) const
{
  if (counter_index >= this->_hardware_live_counters.size()) {
    throw LiveEventCounterOutOfBoundsAccessError{ this->_hardware_live_counters.size(), counter_index };
  }

  return this->_hardware_live_counters[counter_index].read_live();
}

std::optional<double>
perf::EventCounter::live_result(const std::uint64_t counter_index, const std::uint64_t normalization) const
{
  if (counter_index >= this->_hardware_live_counters.size()) {
    throw LiveEventCounterOutOfBoundsAccessError{ this->_hardware_live_counters.size(), counter_index };
  }

  if (const auto value = this->live_result(counter_index); value.has_value()) {
    return value.value() / static_cast<double>(normalization);
  }

  return std::nullopt;
}

std::vector<std::string_view>
perf::EventCounter::live_event_names() const
{
  auto names = std::vector<std::string_view>{};
  std::transform(this->_requested_live_event_set.begin(),
                 this->_requested_live_event_set.end(),
                 std::back_inserter(names),
                 [](const auto& event) { return event.event_name(); });
  return names;
}

perf::LiveEventCounter::LiveEventCounter(const EventCounter& event_counter)
  : _event_counter(event_counter)
  , _event_names(event_counter.live_event_names())
{
  this->_counter_values.resize(this->_event_names.size(), { std::nullopt, std::nullopt });
}

void
perf::LiveEventCounter::start()
{
  for (auto counter_id = 0U; counter_id < this->_counter_values.size(); ++counter_id) {
    /// Set <start value> to current value.
    this->_counter_values[counter_id].first = this->_event_counter.live_result(counter_id);
  }
}

void
perf::LiveEventCounter::stop()
{
  for (auto counter_id = 0U; counter_id < this->_counter_values.size(); ++counter_id) {
    /// Set <stop value> to current value.
    this->_counter_values[counter_id].second = this->_event_counter.live_result(counter_id);
  }
}

double
perf::LiveEventCounter::get(const std::string_view event_name) const
{
  for (auto event_index = 0U; event_index < this->_event_names.size(); ++event_index) {
    /// Find the event matching the given name.
    if (this->_event_names[event_index] == event_name) {

      if (const auto [start_value, stop_value] = this->_counter_values[event_index];
          start_value.has_value() && stop_value.has_value()) {
        /// Calculate the difference by <stop value> - <start value>.
        return stop_value.value() - start_value.value();
      }

      return .0;
    }
  }

  return .0;
}

double
perf::LiveEventCounter::get(const std::string_view event_name, const std::uint64_t normalization) const
{
  return this->get(event_name) / static_cast<double>(normalization);
}

void
perf::MultiEventCounterBase::add(std::string&& event_name, const EventCounter::Schedule schedule)
{
  /// Add the event to every event counter.
  for (auto& event_counter : this->event_counters()) {
    event_counter.add(event_name, schedule);
  }
}

void
perf::MultiEventCounterBase::add(const std::vector<std::string>& event_names, const EventCounter::Schedule schedule)
{
  /// Add the event to every sub event counter.
  for (auto& event_counter : this->event_counters()) {
    event_counter.add(event_names, schedule);
  }
}

void
perf::MultiEventCounterBase::stop()
{
  /// Stop every sub event counter.
  for (auto& event_counter : this->event_counters()) {
    event_counter.stop();
  }
}

void
perf::MultiEventCounterBase::close()
{
  /// Close every sub event counter.
  for (auto& event_counter : this->event_counters()) {
    event_counter.close();
  }
}

perf::CounterResult
perf::MultiEventCounterBase::result(const std::uint64_t normalization) const
{
  if (this->event_counters().empty()) {
    return CounterResult{};
  }

  /// Aggregate hardware events from all individual EventCounters and turn into a list of requested values.

  /// The reference_event_counter is used to access counters (all EventCounters from the list are required to have the
  /// same events but different values).
  const auto& reference_event_counter = this->event_counters().front();
  const auto& reference_event_set = reference_event_counter._requested_event_set;

  /// Build one result of only hardware-event values over all EventCounters by aggregating their values.
  auto aggregated_event_values = std::vector<std::pair<std::string_view, double>>{};
  aggregated_event_values.reserve(reference_event_set.size());

  /// Accumulate all hardware and time events from EventCounters.
  for (const auto& event : reference_event_set) {
    /// Hardware events are read via hardware counter (groups).
    if (event.is_hardware_event()) {
      if (auto scheduled_group = event.scheduled_group(); scheduled_group.has_value()) {

        /// Add up the values from all individual EventCounters in event_counters.
        const auto aggregated_value =
          std::accumulate(this->event_counters().cbegin(),
                          this->event_counters().cend(),
                          .0,
                          [group_id = scheduled_group->id(),
                           in_group_position = scheduled_group->position()](const auto sum, const auto& event_counter) {
                            const auto& group = std::get<0>(event_counter._hardware_event_groups[group_id]);
                            return sum + group.get(in_group_position);
                          });

        /// Add to the aggregated results.
        aggregated_event_values.emplace_back(event.event_name(), aggregated_value);
      }
    }
    /// Time events are read via event counter's start and stop timestamps.
    else if (event.is_time_event()) {
      if (const auto time_event = reference_event_counter._counter_definition.time_event(event.event_name());
          time_event.has_value()) {
        /// Aggregate the values from all individual EventCounters in event_counters.
        const auto aggregated_value = std::accumulate(
          this->event_counters().cbegin(),
          this->event_counters().cend(),
          .0,
          [&time_calculator = std::get<1>(time_event.value())](const auto sum, const auto& event_counter) {
            const auto [start_timestamp, stop_timestamp] = event_counter._start_and_end_time;
            return sum + time_calculator.calculate(start_timestamp, stop_timestamp);
          });

        /// Add to the aggregated results.
        aggregated_event_values.emplace_back(event.event_name(), aggregated_value);
      }
    }
  }

  /// Turn the result of only aggregated hardware events into a result containing requested hardware events and metrics
  /// (which are calculated from hardware events).
  return reference_event_set.result(
    reference_event_counter._counter_definition, CounterResult{ std::move(aggregated_event_values) }, normalization);
}

void
perf::StartableMultiEventCounterBase::start()
{
  /// Start every sub event counter.
  for (auto& event_counter : this->event_counters()) {
    event_counter.start();
  }
}

perf::MultiThreadEventCounter::MultiThreadEventCounter(const CounterDefinition& counter_definition,
                                                       const std::uint16_t num_threads,
                                                       const Config config)
{
  this->_thread_local_counter.reserve(num_threads);
  for (auto thread_index = 0U; thread_index < num_threads; ++thread_index) {
    this->_thread_local_counter.emplace_back(counter_definition, config);
  }
}

perf::MultiThreadEventCounter::MultiThreadEventCounter(EventCounter&& event_counter, const std::uint16_t num_threads)
{
  if (num_threads > 0U) {
    this->_thread_local_counter.reserve(num_threads);
    for (auto thread_index = 0U; thread_index < num_threads - 1U; ++thread_index) {
      this->_thread_local_counter.push_back(EventCounter::copy_from_template(event_counter));
    }
    this->_thread_local_counter.emplace_back(std::move(event_counter));
  }
}

perf::MultiProcessEventCounter::MultiProcessEventCounter(const CounterDefinition& counter_definition,
                                                         std::vector<pid_t>&& process_ids,
                                                         Config config)
{
  this->_process_local_counter.reserve(process_ids.size());

  for (const auto process_id : process_ids) {
    config.process(Process{ process_id });
    this->_process_local_counter.emplace_back(counter_definition, config);
  }
}

perf::MultiProcessEventCounter::MultiProcessEventCounter(EventCounter&& event_counter, std::vector<pid_t>&& process_ids)
{
  if (!process_ids.empty()) {
    this->_process_local_counter.reserve(process_ids.size());
    auto config = event_counter.config();

    for (auto i = 0U; i < process_ids.size() - 1U; ++i) {

      /// Create one counter for every process: Copy the config for every process and bind the EventCounter to that
      /// process.
      config.process(Process{ process_ids[i] });
      auto process_local_counter = EventCounter::copy_from_template(event_counter);
      process_local_counter.config(config);

      this->_process_local_counter.emplace_back(std::move(process_local_counter));
    }

    /// Re-use the given EventCounter for the last process in the list.
    config.process(Process{ process_ids.back() });
    event_counter.config(config);
    this->_process_local_counter.emplace_back(std::move(event_counter));
  }
}

std::optional<perf::CounterResult>
perf::MultiProcessEventCounter::result_of_process(const pid_t process_id, const std::uint64_t normalization) const
{
  for (const auto& event_counter : this->_process_local_counter) {
    if (event_counter.config().process() == process_id) {
      return event_counter.result(normalization);
    }
  }

  return std::nullopt;
}

perf::MultiCoreEventCounter::MultiCoreEventCounter(const CounterDefinition& counter_definition,
                                                   std::vector<std::uint16_t>&& cpu_ids,
                                                   Config config)
{
  config.process(Process::Any); /// Record every thread/process on the given CPUs.

  this->_cpu_local_counter.reserve(cpu_ids.size());

  for (const auto cpu_id : cpu_ids) {
    config.cpu_core(CpuCore{ cpu_id });
    this->_cpu_local_counter.emplace_back(counter_definition, config);
  }
}

perf::MultiCoreEventCounter::MultiCoreEventCounter(EventCounter&& event_counter, std::vector<std::uint16_t>&& cpu_ids)
{
  this->_cpu_local_counter.reserve(cpu_ids.size());
  auto config = event_counter.config();
  config.process(Process::Any); /// Record every thread/process on the given CPUs.

  if (!cpu_ids.empty()) {
    for (auto i = 0U; i < cpu_ids.size() - 1U; ++i) {

      /// Create one EventCounter for every CPU core from the list via config.
      config.cpu_core(CpuCore{ cpu_ids[i] });
      auto process_local_counter = EventCounter::copy_from_template(event_counter);
      process_local_counter.config(config);

      this->_cpu_local_counter.push_back(std::move(process_local_counter));
    }

    /// Re-use the given EventCounter for the last CPU Id in the list.
    config.cpu_core(CpuCore{ cpu_ids.back() });
    event_counter.config(config);
    this->_cpu_local_counter.push_back(std::move(event_counter));
  }
}

std::optional<perf::CounterResult>
perf::MultiCoreEventCounter::result_of_core(const std::uint16_t core_id, const std::uint64_t normalization) const
{
  for (const auto& event_counter : this->_cpu_local_counter) {
    if (event_counter.config().cpu_core() == core_id) {
      return event_counter.result(normalization);
    }
  }

  return std::nullopt;
}