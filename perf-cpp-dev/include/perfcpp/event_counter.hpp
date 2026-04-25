#pragma once

#include <chrono>
#include <optional>
#include <perfcpp/counter/config.hpp>
#include <perfcpp/counter/counter.hpp>
#include <perfcpp/counter/group.hpp>
#include <perfcpp/counter/requested_event.hpp>
#include <perfcpp/counter_definition.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace perf {
/**
 * The EventCounter allows to specify events that should be counted by hardware performance counters and start/stop
 * counting.
 */
class EventCounter
{
  friend class MultiEventCounterBase;

public:
  /**
   * Mode how to schedule multiple events to physical hardware counters.
   */
  enum class Schedule : std::uint8_t
  {
    Append,   /// Events are placed on any hardware counter.
    Separate, /// Events are placed on a separate hardware counters.
    Group     /// Events are placed on the same hardware counter.
  };

  /**
   * Creates a copy from another event counter. The copy will hold the same counter definition, config, event sets, and
   * counter configurations. However, the copy will not be opened (this is also true for the counters).
   *
   * @param other EventCounter to copy from.
   * @return Copied event counter.
   */
  [[nodiscard]] static EventCounter copy_from_template(const EventCounter& other);

  explicit EventCounter(const CounterDefinition& counter_definition, Config config = {})
    : _counter_definition(counter_definition)
    , _config(config)
  {
  }

  explicit EventCounter(Config config = {})
    : EventCounter(CounterDefinition::global(), config)
  {
  }

  EventCounter(const EventCounter&) = delete;
  EventCounter(EventCounter&&) noexcept = default;

  ~EventCounter() noexcept(false);

  EventCounter& operator=(const EventCounter&) = delete;
  EventCounter& operator=(EventCounter&&) noexcept = delete;

  /**
   * Add the specified event to the list of countered performance events.
   * The event must exist within the counter definitions.
   *
   * @param event_name Name of the event.
   * @param schedule Request to schedule events anywhere (append), or to a single hardware counter (individual).
   */
  void add(std::string&& event_name, const Schedule schedule = Schedule::Append) { add(event_name, schedule); }

  /**
   * Add the specified event to the list of monitored performance events.
   * The event must exist within the counter definitions.
   *
   * @param event_name Name of the event.
   * @param schedule Request to schedule events anywhere (append), or to a single hardware counter (individual).
   */
  void add(const std::string& event_name, Schedule schedule = Schedule::Append);

  /**
   * Add the specified events to the list of countered events.
   * The events must exist within the counter definitions.
   *
   * @param event_names List of names of the events.
   * @param schedule Request to schedule events anywhere (append), or to a single hardware counter (individual), or as a
   * group (all to the same hardware counter).
   */
  void add(std::vector<std::string>&& event_names, const Schedule schedule = Schedule::Append)
  {
    add(event_names, schedule);
  }

  /**
   * Add the specified events to the list of countered performance events.
   * The events must exist within the counter definitions.
   *
   * @param event_names List of names of the counted events.
   * @param schedule Request to schedule events anywhere (append), or to a single hardware counter (individual), or as a
   * group (all to the same hardware counter).
   */
  void add(const std::vector<std::string>& event_names, Schedule schedule = Schedule::Append);

  /**
   * Add the specified event to the list of countered performance events.
   * The event can be read "live" without stopping the counter (only x86 hardware).
   * The event must exist within the counter definitions.
   *
   * @param event_name Name of the event.
   */
  void add_live(std::string&& event_name) { add_live(event_name); }

  /**
   * Add the specified event to the list of countered performance events.
   * The event can be read "live" without stopping the counter (only x86 hardware).
   * The event must exist within the counter definitions.
   *
   * @param event_name Name of the event.
   */
  void add_live(const std::string& event_name);

  /**
   * Add the specified events to the list of countered performance events.
   * The events can be read "live" without stopping the counter (only x86 hardware).
   * The events must exist within the counter definitions.
   *
   * @param event_names List of event names.
   */
  void add_live(std::vector<std::string>&& event_names);

  /**
   * Opens hardware performance counters.
   */
  void open();

  /**
   * Opens (if not already done) and starts recording performance counters.
   */
  void start();

  /**
   * Stops recording performance counters.
   */
  void stop();

  /**
   * Closes the hardware performance counters.
   */
  void close();

  /**
   * Returns the result of the performance measurement.
   *
   * @param normalization Normalization value, default = 1.
   * @return List of event names and values.
   */
  [[nodiscard]] CounterResult result(std::uint64_t normalization = 1U) const;

  /**
   * Performs a live read for every group without stopping the counter and writes it into the result input/output.
   * The reason for having an output parameter is to not allocate any memory during a lightweight read.
   *
   * @param result Output parameter to write the result without allocating any memory.
   */
  void live_result(std::vector<double>& result) const;

  /**
   * Performs a live read for every group without stopping the counter and writes it into the result input/output.
   * The reason for having an output parameter is to not allocate any memory during a lightweight read.
   *
   * @param result Output parameter to write the result without allocating any memory.
   * @param normalization  Normalization value.
   */
  void live_result(std::vector<double>& result, std::uint64_t normalization) const;

  /**
   * Performs a live read for every group without stopping the counter.
   *
   * @param counter_index Index of the counter to be read live.
   * @return The live value of the counter.
   */
  [[nodiscard]] std::optional<double> live_result(std::uint64_t counter_index) const;

  /**
   * Performs a live read for every group without stopping the counter.
   *
   * @param counter_index Index of the counter to be read live.
   * @param normalization Normalization value.
   * @return The live value of the counter.
   */
  [[nodiscard]] std::optional<double> live_result(std::uint64_t counter_index, std::uint64_t normalization) const;

  /**
   * @return A list of event names that are added as live events.
   */
  [[nodiscard]] std::vector<std::string_view> live_event_names() const;

  /**
   * @return Configuration of the counter.
   */
  [[nodiscard]] const Config& config() const noexcept { return _config; }

  /**
   * Update the configuration of the counter.
   *
   * @param config New config.
   */
  void config(const Config config) noexcept { _config = config; }

private:
  /// List of event names and codes.
  const CounterDefinition& _counter_definition;

  /// The configuration of counters (include user, kernel, etc.).
  Config _config;

  /// List of requested events and metrics that are added to groups. This list is only to track the order and
  /// configuration of the user's requested events.
  RequestedEventSet _requested_event_set;

  /// List of requested live events. This list is only to track the order and
  /// configuration of the user's request.
  RequestedEventSet _requested_live_event_set;

  /// Hardware counter groups holding performance counters that are started, stopped, and read. The bool indicates if
  /// that group is "open", meaning no counter can or should be added (false), because the group is full or the user
  /// wanted to schedule the counters together without any other.
  std::vector<std::pair<Group, bool>> _hardware_event_groups;

  /// Number of groups in _hardware_event_groups that are dedicated to fixed-function PMCs.
  /// Fixed-PMC groups bypass the generic PMC limit and are excluded from size().
  std::size_t _num_fixed_groups{ 0U };

  /// List of counters that are marked to be read "live" (without stopping) using the "rdpmc" instruction (only
  /// implemented on x86 hardware).
  std::vector<Counter> _hardware_live_counters;

  /// Start and stop time points for time events.
  std::pair<std::chrono::steady_clock::time_point, std::chrono::steady_clock::time_point> _start_and_end_time;

  /// Flag indicating if the EventCounter was opened. Opens automatically on startup at the latest.
  bool _is_opened{ false };

  EventCounter(const CounterDefinition& counter_definition,
               const Config config,
               RequestedEventSet requested_event_set,
               RequestedEventSet requested_live_event_set)
    : _counter_definition(counter_definition)
    , _config(config)
    , _requested_event_set(std::move(requested_event_set))
    , _requested_live_event_set(std::move(requested_live_event_set))
  {
  }

  /**
   * @return The number of opened (or to open) counters (groups or group leaders and live counters).
   */
  [[nodiscard]] std::size_t size() const noexcept
  {
    return _hardware_event_groups.size() - _num_fixed_groups + _hardware_live_counters.size();
  }

  /**
   * Extracts information (counter/metric name, hardware counter configuration, flag if is included into events) from
   * the given event into a given result vector. The result vector can be used to schedule the events, based on the user
   * request.
   *
   * @param name Name of the event to add.
   * @param is_visible_in_results Indicates if the added event/metric/time should be visible in the results.
   * @param events List to extend the requested events. If the event is a single hardware event, the list will
   * have one entry. If the event is a metric, the list will have multiple entries.
   */
  void expand_to_events(const std::string& name,
                        bool is_visible_in_results,
                        std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>& events) const;

  /**
   * Adds the provided event to the given result vector.
   * If the event is already in the result vector, only the visibility (is_shown_in_results) will be adjusted.
   *
   * @param pmu_name Name of the PMU.
   * @param event_name Name of the event.
   * @param event_config Configuration of the counter.
   * @param is_shown_in_results Visibility.
   * @param requested_events List of requested events.
   */
  static void add(std::string_view pmu_name,
                  std::string_view event_name,
                  const CounterConfig& event_config,
                  bool is_shown_in_results,
                  std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>& requested_events);

  /**
   * Schedules the given events based on the request into hardware groups and places the event names in the
   * user-requested event set. If the events do not fit (e.g., based on the request; too many counters requested to be
   * placed on the same hardware counter), the method will throw an exception to let the user know.
   *
   * @param events List of events to schedule.
   * @param schedule Request of the user.
   */
  void schedule(std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>&& events, Schedule schedule);

  /**
   * Schedules all fixed-function PMC events from the given event list into dedicated pinned groups.
   * Fixed events are removed from the list; the remaining events are returned for further scheduling.
   * Only has an effect on Intel processors with fixed-function PMCs.
   *
   * @param events List of events; fixed events will be scheduled and removed.
   * @return The remaining events after fixed events have been extracted.
   */
  [[nodiscard]] std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>
  schedule_to_fixed_hardware_counters(std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>&& events);

  /**
   * Schedules the given events to generic hardware counter groups.
   * Depending on the schedule mode, events are appended to existing groups (Append) or placed in their own group
   * (Separate).
   *
   * @param events List of events to schedule.
   * @param schedule Append or Separate.
   */
  void schedule_to_generic_hardware_counters(
    std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>&& events,
    Schedule schedule);

  /**
   * Schedules all given events into a single hardware counter group.
   * Throws if the group limit is reached or the events do not fit into one group.
   *
   * @param events List of events to schedule into one group.
   */
  void schedule_as_group(std::vector<std::pair<RequestedEvent, std::optional<CounterConfig>>>&& events);

  /**
   * Try to append the given event to any hardware counter.
   *
   * @param event Event to append.
   * @param event_config Configuration of the event.
   * @return True, if the event could be appended to any hardware counter. False, otherwise.
   */
  [[nodiscard]] bool append_to_any_hardware_counter(RequestedEvent& event, const CounterConfig& event_config);

  /**
   * Tries to create a new group and appends the given event.
   *
   * @param event Event to append.
   * @param event_config Configuration of the event.
   * @param is_keep_open If true, further events can be added in the future. Otherwise, the event will be the only one.
   */
  void create_new_group(RequestedEvent& event, const CounterConfig& event_config, bool is_keep_open);
};

/**
 * The LiveEventCounter grants access to live events of an existing EventCounter.
 * While live events can be read every time–using EventCounter–, this class enables to "start" (read the current value),
 * "stop" (read the current value again), and calculate the difference as a result without allocating memory for when
 * reading the values.
 */
class LiveEventCounter
{
public:
  explicit LiveEventCounter(const EventCounter& event_counter);
  LiveEventCounter(const LiveEventCounter&) = default;
  LiveEventCounter(LiveEventCounter&&) noexcept = default;
  ~LiveEventCounter() = default;
  LiveEventCounter& operator=(const LiveEventCounter&) = delete;
  LiveEventCounter& operator=(LiveEventCounter&&) noexcept = delete;

  /**
   * Retrieves the current value for every live counter and mark them as "start" value.
   */
  void start();

  /**
   * Retrieves the current value for every live counter and mark them as "stop" value.
   */
  void stop();

  /**
   * Calculates the difference between the start- and the stop values for the live event with the given name.
   * Returns 0 if the event name was not found.
   *
   * @param event_name Event to calculate the stop - start value for.
   * @return The difference between the stop and the start value, or 0 if the name was not found.
   */
  [[nodiscard]] double get(std::string_view event_name) const;

  /**
   * Calculates the difference between the start- and the stop values for the live event with the given name.
   * Returns 0 if the event name was not found.
   *
   * @param event_name Event to calculate the stop - start value for.
   * @param normalization Value to normalize the results.
   * @return The difference between the stop and the start value divided by the normalization value; or 0 if the name
   * was not found.
   */
  [[nodiscard]] double get(std::string_view event_name, std::uint64_t normalization) const;

private:
  /// EventCounter to access live events.
  const EventCounter& _event_counter;

  /// List of live events, received via EventCounter.
  std::vector<std::string_view> _event_names;

  /// List of (start, stop) tuples for all live events.
  std::vector<std::pair<std::optional<double>, std::optional<double>>> _counter_values;
};

class MultiEventCounterBase
{
public:
  MultiEventCounterBase() noexcept = default;
  MultiEventCounterBase(const MultiEventCounterBase&) = default;
  MultiEventCounterBase(MultiEventCounterBase&&) noexcept = default;
  virtual ~MultiEventCounterBase() = default;
  MultiEventCounterBase& operator=(const MultiEventCounterBase&) = default;
  MultiEventCounterBase& operator=(MultiEventCounterBase&&) noexcept = default;

  /**
   * Add the specified event to the list of countered performance events.
   * The event must exist within the counter definitions.
   *
   * @param event_name Name of the event.
   * @param schedule Request to schedule events anywhere (append), or to a single hardware counter (individual).
   */
  void add(std::string&& event_name, EventCounter::Schedule schedule = EventCounter::Schedule::Append);

  /**
   * Add the specified event to the list of monitored performance events.
   * The event must exist within the counter definitions.
   *
   * @param event_name Name of the event.
   * @param schedule Request to schedule events anywhere (append), or to a single hardware counter (individual).
   */
  void add(const std::string& event_name, const EventCounter::Schedule schedule = EventCounter::Schedule::Append)
  {
    add(std::string{ event_name }, schedule);
  }

  /**
   * Add the specified events to the list of monitored performance events.
   * The events must exist within the counter definitions.
   *
   * @param event_names List of names of the events.
   * @param schedule Request to schedule events anywhere (append), or to a single hardware counter (individual), or as a
   * group (all to the same hardware counter).
   */
  void add(std::vector<std::string>&& event_names,
           const EventCounter::Schedule schedule = EventCounter::Schedule::Append)
  {
    add(event_names, schedule);
  }

  /**
   * Add the specified events to the list of monitored performance events.
   * The events must exist within the counter definitions.
   *
   * @param event_names List of names of the events.
   * @param schedule Request to schedule events anywhere (append), or to a single hardware counter (individual), or as a
   * group (all to the same hardware counter).
   */
  void add(const std::vector<std::string>& event_names,
           EventCounter::Schedule schedule = EventCounter::Schedule::Append);

  /**
   * Stops recording performance counters.
   */
  void stop();

  /**
   * Closes the hardware performance counters.
   */
  void close();

  /**
   * Returns the result of the performance measurement.
   *
   * @param normalization Normalization value, default = 1.
   * @return List of counter names and values.
   */
  [[nodiscard]] CounterResult result(std::uint64_t normalization = 1U) const;

protected:
  [[nodiscard]] virtual std::vector<EventCounter>& event_counters() noexcept = 0;
  [[nodiscard]] virtual const std::vector<EventCounter>& event_counters() const noexcept = 0;
};

class StartableMultiEventCounterBase : public MultiEventCounterBase
{
public:
  StartableMultiEventCounterBase() noexcept = default;
  StartableMultiEventCounterBase(const StartableMultiEventCounterBase&) = default;
  StartableMultiEventCounterBase(StartableMultiEventCounterBase&&) noexcept = default;
  ~StartableMultiEventCounterBase() override = default;
  StartableMultiEventCounterBase& operator=(const StartableMultiEventCounterBase&) = default;
  StartableMultiEventCounterBase& operator=(StartableMultiEventCounterBase&&) noexcept = default;

  /**
   * Opens and starts all event counters.
   */
  void start();
};

/**
 * Wrapper for EventCounter to record counters on different user-level threads.
 * Each thread can start/stop its own counter.
 * The results can be aggregated or queried for a specific thread.
 */
class MultiThreadEventCounter final : public MultiEventCounterBase
{
public:
  MultiThreadEventCounter(const CounterDefinition& counter_definition, std::uint16_t num_threads, Config config = {});

  explicit MultiThreadEventCounter(const std::uint16_t num_threads, const Config config = {})
    : MultiThreadEventCounter(CounterDefinition::global(), num_threads, config)
  {
  }

  MultiThreadEventCounter(EventCounter&& event_counter, std::uint16_t num_threads);

  MultiThreadEventCounter(const EventCounter& event_counter, const std::uint16_t num_threads)
    : MultiThreadEventCounter(EventCounter::copy_from_template(event_counter), num_threads)
  {
  }

  MultiThreadEventCounter(const MultiThreadEventCounter&) = delete;
  MultiThreadEventCounter(MultiThreadEventCounter&&) noexcept = default;

  ~MultiThreadEventCounter() override { this->close(); }

  MultiThreadEventCounter& operator=(const MultiThreadEventCounter&) = delete;
  MultiThreadEventCounter& operator=(MultiThreadEventCounter&&) noexcept = default;

  /**
   * Opens and starts recording performance counters for the given thread.
   *
   * @param thread_id Id of the thread.
   */
  void start(std::uint16_t thread_id)
  {
    if (thread_id >= _thread_local_counter.size()) {
      throw ThreadIdOutOfBoundsError{ thread_id };
    }

    _thread_local_counter[thread_id].start();
  }

  /**
   * Stops and closes recording performance counters.
   *
   * @param thread_id Id of the thread.
   */
  void stop(std::uint16_t thread_id)
  {
    if (thread_id >= _thread_local_counter.size()) {
      throw ThreadIdOutOfBoundsError{ thread_id };
    }

    _thread_local_counter[thread_id].stop();
  }

  /// Expose the base-class stop() so callers can stop all threads at once from the main thread.
  using MultiEventCounterBase::stop;

  /**
   * Returns the result of the performance measurement for a given thread.
   *
   * @param thread_id Id of the thread.
   * @param normalization Normalization value, default = 1.
   * @return List of counter names and values.
   */
  [[nodiscard]] CounterResult result_of_thread(const std::uint16_t thread_id, std::uint64_t normalization = 1U) const
  {
    if (thread_id >= _thread_local_counter.size()) {
      throw ThreadIdOutOfBoundsError{ thread_id };
    }

    return _thread_local_counter[thread_id].result(normalization);
  }

private:
  std::vector<perf::EventCounter> _thread_local_counter;

  [[nodiscard]] std::vector<EventCounter>& event_counters() noexcept override { return _thread_local_counter; }
  [[nodiscard]] const std::vector<EventCounter>& event_counters() const noexcept override
  {
    return _thread_local_counter;
  }
};

/**
 * Wrapper for EventCounter to record counters on different process ids (i.e., linux thread ids).
 * ProcessIds / ThreadIds have to be specified. The counter can be started/stopped at once.
 * The results will be aggregated.
 */
class MultiProcessEventCounter final : public StartableMultiEventCounterBase
{
public:
  MultiProcessEventCounter(const CounterDefinition& counter_definition,
                           std::vector<pid_t>&& process_ids,
                           Config config = {});

  MultiProcessEventCounter(const CounterDefinition& counter_definition,
                           const std::vector<pid_t>& process_ids,
                           Config config = {})
    : MultiProcessEventCounter(counter_definition, std::vector<pid_t>{ process_ids }, config)
  {
  }

  explicit MultiProcessEventCounter(std::vector<pid_t>&& process_ids, const Config config = {})
    : MultiProcessEventCounter(CounterDefinition::global(), std::move(process_ids), config)
  {
  }

  explicit MultiProcessEventCounter(const std::vector<pid_t>& process_ids, const Config config = {})
    : MultiProcessEventCounter(CounterDefinition::global(), std::vector<pid_t>{ process_ids }, config)
  {
  }

  MultiProcessEventCounter(EventCounter&& event_counter, std::vector<pid_t>&& process_ids);

  MultiProcessEventCounter(const EventCounter& event_counter, std::vector<pid_t>&& process_ids)
    : MultiProcessEventCounter(EventCounter::copy_from_template(event_counter), std::move(process_ids))
  {
  }

  MultiProcessEventCounter(const EventCounter& event_counter, const std::vector<pid_t>& process_ids)
    : MultiProcessEventCounter(EventCounter::copy_from_template(event_counter), std::vector<pid_t>{ process_ids })
  {
  }

  MultiProcessEventCounter(const MultiProcessEventCounter&) = delete;
  MultiProcessEventCounter(MultiProcessEventCounter&&) noexcept = default;

  ~MultiProcessEventCounter() override { this->close(); }

  MultiProcessEventCounter& operator=(const MultiProcessEventCounter&) = delete;
  MultiProcessEventCounter& operator=(MultiProcessEventCounter&&) noexcept = default;

  /**
   * Returns the result of the performance measurement for a given process.
   *
   * @param process_id Id of the process.
   * @param normalization Normalization value, default = 1.
   * @return List of counter names and values.
   */
  [[nodiscard]] std::optional<CounterResult> result_of_process(pid_t process_id,
                                                               std::uint64_t normalization = 1U) const;

private:
  std::vector<EventCounter> _process_local_counter;

  [[nodiscard]] std::vector<EventCounter>& event_counters() noexcept override { return _process_local_counter; }
  [[nodiscard]] const std::vector<EventCounter>& event_counters() const noexcept override
  {
    return _process_local_counter;
  }
};

/**
 * Wrapper for EventCounter to record counters on different CPU cores.
 * CPU ids have to be specified. The counter can be started/stopped at once.
 * The results will be aggregated.
 */
class MultiCoreEventCounter final : public StartableMultiEventCounterBase
{
public:
  MultiCoreEventCounter(const CounterDefinition& counter_definition,
                        std::vector<std::uint16_t>&& cpu_ids,
                        Config config = {});

  MultiCoreEventCounter(const CounterDefinition& counter_definition,
                        const std::vector<std::uint16_t>& cpu_ids,
                        Config config = {})
    : MultiCoreEventCounter(counter_definition, std::vector<std::uint16_t>{ cpu_ids }, config)
  {
  }

  explicit MultiCoreEventCounter(std::vector<std::uint16_t>&& cpu_ids, const Config config = {})
    : MultiCoreEventCounter(CounterDefinition::global(), std::move(cpu_ids), config)
  {
  }

  explicit MultiCoreEventCounter(const std::vector<std::uint16_t>& cpu_ids, const Config config = {})
    : MultiCoreEventCounter(CounterDefinition::global(), std::vector<std::uint16_t>{ cpu_ids }, config)
  {
  }

  MultiCoreEventCounter(EventCounter&& event_counter, std::vector<std::uint16_t>&& cpu_ids);

  MultiCoreEventCounter(const EventCounter& event_counter, std::vector<std::uint16_t>&& cpu_ids)
    : MultiCoreEventCounter(EventCounter::copy_from_template(event_counter), std::move(cpu_ids))
  {
  }

  MultiCoreEventCounter(const EventCounter& event_counter, const std::vector<std::uint16_t>& cpu_ids)
    : MultiCoreEventCounter(EventCounter::copy_from_template(event_counter), std::vector<std::uint16_t>{ cpu_ids })
  {
  }

  MultiCoreEventCounter(const MultiCoreEventCounter&) = delete;
  MultiCoreEventCounter(MultiCoreEventCounter&&) noexcept = default;

  ~MultiCoreEventCounter() override { this->close(); }

  MultiCoreEventCounter& operator=(const MultiCoreEventCounter&) = delete;
  MultiCoreEventCounter& operator=(MultiCoreEventCounter&&) noexcept = default;

  /**
   * Returns the result of the performance measurement for a given CPU core.
   *
   * @param core_id Id of the CPU core.
   * @param normalization Normalization value, default = 1.
   * @return List of counter names and values.
   */
  [[nodiscard]] std::optional<CounterResult> result_of_core(std::uint16_t core_id,
                                                            std::uint64_t normalization = 1U) const;

private:
  std::vector<EventCounter> _cpu_local_counter;

  [[nodiscard]] std::vector<EventCounter>& event_counters() noexcept override { return _cpu_local_counter; }
  [[nodiscard]] const std::vector<EventCounter>& event_counters() const noexcept override { return _cpu_local_counter; }
};
}