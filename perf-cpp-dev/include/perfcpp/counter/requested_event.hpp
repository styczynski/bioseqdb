#pragma once

#include <cstdint>
#include <optional>
#include <perfcpp/counter/counter.hpp>
#include <perfcpp/counter_definition.hpp>
#include <perfcpp/util/graph.hpp>
#include <string_view>
#include <vector>

namespace perf {
/**
 * The RequestedEvent class stores information about events that will be recorded by the EventCounter and Sampler
 * The information include the group an event is scheduled to, the index within the group, the name, and a flag if the
 * event should be shown within the results (which is not true for events only needed for metrics). The EventCounter and
 * Sampler will have an ordered list of events, dictating the order the user requested the events to output the events
 * in exactly that order.
 */
class RequestedEvent
{
public:
  class ScheduledHardwareCounterGroup
  {
  public:
    explicit ScheduledHardwareCounterGroup(const std::uint8_t position) noexcept
      : _position(position)
    {
    }
    ScheduledHardwareCounterGroup(const std::uint8_t id, const std::uint8_t position) noexcept
      : _id(id)
      , _position(position)
    {
    }
    ScheduledHardwareCounterGroup(const ScheduledHardwareCounterGroup&) = default;
    ScheduledHardwareCounterGroup(ScheduledHardwareCounterGroup&&) noexcept = default;
    ~ScheduledHardwareCounterGroup() noexcept = default;
    ScheduledHardwareCounterGroup& operator=(const ScheduledHardwareCounterGroup&) = default;
    ScheduledHardwareCounterGroup& operator=(ScheduledHardwareCounterGroup&&) noexcept = default;

    [[nodiscard]] std::uint8_t id() const noexcept { return _id; }
    [[nodiscard]] std::uint8_t position() const noexcept { return _position; }

  private:
    std::uint8_t _id{ 0U };
    std::uint8_t _position;
  };

  enum class Type : std::uint8_t
  {
    HardwareEvent,
    Metric,
    TimeEvent
  };

  RequestedEvent(const std::optional<std::string_view> pmu_name,
                 const std::string_view event_name,
                 const bool is_shown_in_results,
                 const Type type,
                 const std::optional<ScheduledHardwareCounterGroup> scheduled_group = std::nullopt) noexcept
    : _pmu_name(pmu_name)
    , _event_name(event_name)
    , _is_shown_in_results(is_shown_in_results)
    , _type(type)
    , _scheduled_hardware_counter_group(scheduled_group)
  {
  }

  RequestedEvent(const std::optional<std::string_view> pmu_name,
                 const std::string_view event_name,
                 const bool is_shown_in_results,
                 const std::uint8_t group_id,
                 const std::uint8_t position) noexcept
    : RequestedEvent(pmu_name,
                     event_name,
                     is_shown_in_results,
                     Type::HardwareEvent,
                     ScheduledHardwareCounterGroup{ group_id, position })
  {
  }

  RequestedEvent(const std::optional<std::string_view> pmu_name,
                 const std::string_view event_name,
                 const std::uint8_t group_id,
                 const std::uint8_t position) noexcept
    : RequestedEvent(pmu_name, event_name, true, group_id, position)
  {
  }

  RequestedEvent(const std::string_view event_name, const bool is_shown_in_results, const Type type) noexcept
    : _event_name(event_name)
    , _is_shown_in_results(is_shown_in_results)
    , _type(type)
  {
  }

  RequestedEvent(const RequestedEvent&) = default;
  RequestedEvent(RequestedEvent&&) noexcept = default;

  ~RequestedEvent() = default;

  RequestedEvent& operator=(const RequestedEvent&) = default;
  RequestedEvent& operator=(RequestedEvent&&) noexcept = default;

  [[nodiscard]] std::optional<std::string_view> pmu_name() const noexcept { return _pmu_name; }
  [[nodiscard]] std::string_view event_name() const noexcept { return _event_name; }
  [[nodiscard]] bool is_hardware_event() const noexcept { return _type == Type::HardwareEvent; }
  [[nodiscard]] bool is_metric() const noexcept { return _type == Type::Metric; }
  [[nodiscard]] bool is_time_event() const noexcept { return _type == Type::TimeEvent; }
  [[nodiscard]] bool is_shown_in_results() const noexcept { return _is_shown_in_results; }
  [[nodiscard]] std::optional<ScheduledHardwareCounterGroup> scheduled_group() const noexcept
  {
    return _scheduled_hardware_counter_group;
  }

  void shown_in_results(const bool is_shown_in_results) noexcept { _is_shown_in_results = is_shown_in_results; }
  void scheduled_group(const std::uint8_t group_id, const std::uint8_t position) noexcept
  {
    _scheduled_hardware_counter_group = { group_id, position };
  }

private:
  /// Name of the PMU for hardware events. May be nullopt for metrics and time events.
  std::optional<std::string_view> _pmu_name{ std::nullopt };

  /// Name of the event (references a string in the CounterDefinition).
  std::string_view _event_name;

  /// Indicates that the event is included into results. Some events are "only" requested by metrics and are only
  /// needed for calculating them but are not requested by the user.
  bool _is_shown_in_results;

  /// Type, e.g., hardware event, metric, or time event.
  Type _type;

  /// Position (hardware counter group id and position within that group) the event is scheduled to.
  std::optional<ScheduledHardwareCounterGroup> _scheduled_hardware_counter_group{ std::nullopt };
};

/**
 * The RequestedEventSet manages events that are requested by the user or metrics and takes care that no event is added
 * more than one time (e.g., by the user and a metric).
 */
class RequestedEventSet
{
public:
  RequestedEventSet() = default;
  RequestedEventSet(const RequestedEventSet&) noexcept = default;
  RequestedEventSet(RequestedEventSet&&) noexcept = default;
  explicit RequestedEventSet(const std::size_t capacity) { _requested_events.reserve(capacity); }

  ~RequestedEventSet() = default;

  RequestedEventSet& operator=(const RequestedEventSet&) = default;
  RequestedEventSet& operator=(RequestedEventSet&&) noexcept = default;

  /**
   * Appends an event to the event set, if not present.
   * The event will be interpreted as a hardware event (since it is scheduled to a group) and marked as shown in
   * results.
   *
   * @param event Requested event.
   * @return True, if the event was added. False, if the event was already in the event set.
   */
  bool add(RequestedEvent&& event) { return add(event); }

  /**
   * Appends an event to the event set, if not present.
   * The event will be interpreted as a hardware event (since it is scheduled to a group) and marked as shown in
   * results.
   *
   * @param event Requested event.
   * @return True, if the event was added. False, if the event was already in the event set.
   */
  bool add(const RequestedEvent& event);

  /**
   * Appends an event to the event set, if not present.
   * The event will be interpreted as a hardware event (since it is scheduled to a group) and marked as shown in
   * results.
   *
   * @param event Requested event.
   * @param group_id Id of the group the event was scheduled to.
   * @param position Position within the group the event was scheduled to.
   * @return True, if the event was added. False, if the event was already in the event set.
   */
  bool add(RequestedEvent& event, const std::uint8_t group_id, const std::uint8_t position)
  {
    event.scheduled_group(group_id, position);
    return add(event);
  }

  /**
   * Checks if the event is present in the requested set. If so, set the visibility.
   * Otherwise, return false, indicating that the event needs to be added.
   *
   * @param pmu_name Name of the PMU.
   * @param event_name Name of the event.
   * @param is_shown_in_results The visibility of the event.
   * @return True, if the event is present.
   */
  [[nodiscard]] bool adjust_visibility_if_present(std::optional<std::string_view> pmu_name,
                                                  std::string_view event_name,
                                                  bool is_shown_in_results);

  /**
   * Constructs a CounterResult for a given CounterResult that uses hardware events only.
   * The returned CounterResult will include metrics added to the requested event set.
   *
   * @param counter_definition Counter definition.
   * @param hardware_events_result Result containing only hardware events.
   * @param normalization Value to normalize the (non-metric) results to.
   * @return CounterResult including all events requested as visible; hardware events and metrics.
   */
  [[nodiscard]] CounterResult result(const CounterDefinition& counter_definition,
                                     CounterResult&& hardware_events_result,
                                     std::uint64_t normalization) const;

  /**
   * @return True, if no event was added.
   */
  [[nodiscard]] bool empty() const noexcept { return _requested_events.empty(); }

  /**
   * @return The size of the set.
   */
  [[nodiscard]] std::size_t size() const noexcept { return _requested_events.size(); }

  [[nodiscard]] std::vector<RequestedEvent>::iterator begin() noexcept { return _requested_events.begin(); }
  [[nodiscard]] std::vector<RequestedEvent>::const_iterator begin() const noexcept
  {
    return _requested_events.cbegin();
  }

  [[nodiscard]] std::vector<RequestedEvent>::iterator end() noexcept { return _requested_events.end(); }
  [[nodiscard]] std::vector<RequestedEvent>::const_iterator end() const noexcept { return _requested_events.cend(); }

private:
  /// List dictating the order of events in the set.
  std::vector<RequestedEvent> _requested_events;

  /**
   * Build a directed dependency graph for all metrics available in the requested event set.
   *
   * @param counter_definition Counter definition to lookup metrics.
   * @return A directed graph, connecting dependent metrics.
   */
  [[nodiscard]] util::DirectedGraph<std::string_view> build_metric_graph(
    const CounterDefinition& counter_definition) const;
};
}