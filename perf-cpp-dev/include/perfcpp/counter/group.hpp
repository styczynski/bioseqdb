#pragma once

#include <cstdint>
#include <optional>
#include <perfcpp/counter/config.hpp>
#include <perfcpp/counter/counter.hpp>
#include <perfcpp/sample/recording_values.hpp>
#include <vector>

namespace perf {

/**
 * Format the counter values are stored by the perf subsystem for a single counter group.
 */
template<std::size_t S>
struct CounterValues
{
public:
  using time_t = std::uint64_t;
  using size_t = std::uint64_t;

  class ValueAndIdentifier
  {
  public:
    ValueAndIdentifier() noexcept = default;
    ValueAndIdentifier(const ValueAndIdentifier&) = default;
    ValueAndIdentifier(ValueAndIdentifier&&) noexcept = default;
    ~ValueAndIdentifier() noexcept = default;
    ValueAndIdentifier& operator=(const ValueAndIdentifier&) = default;
    ValueAndIdentifier& operator=(ValueAndIdentifier&&) noexcept = default;

    [[nodiscard]] std::uint64_t value() const noexcept { return _value; }
    [[nodiscard]] std::uint64_t id() const noexcept { return _id; }

  private:
    std::uint64_t _value{ std::numeric_limits<std::uint64_t>::max() };
    std::uint64_t _id{ std::numeric_limits<std::uint64_t>::max() };
  };

  CounterValues() noexcept = default;
  CounterValues(const CounterValues&) = default;
  CounterValues(CounterValues&&) noexcept = default;
  ~CounterValues() noexcept = default;
  CounterValues& operator=(const CounterValues&) = default;
  CounterValues& operator=(CounterValues&&) noexcept = default;

  [[nodiscard]] time_t time_enabled() const noexcept { return _time_enabled; }
  [[nodiscard]] time_t time_running() const noexcept { return _time_running; }

  /**
   * Returns the value of the counter with the specified id.
   *
   * @param id Id to get the value for.
   * @return Value of the specified counter, if the id is present.
   */
  [[nodiscard]] std::optional<std::uint64_t> value(const std::uint64_t id) const noexcept
  {
    for (const auto& value : _values) {
      if (value.id() == id) {
        return value.value();
      }
    }

    return std::nullopt;
  }

private:
  /// Number of counters in the following array.
  [[maybe_unused]] std::size_t _count_members{ 0U };

  /// Time the event was enabled.
  time_t _time_enabled{ 0U };

  /// Time the event was running.
  time_t _time_running{ 0U };

  /// Values of the members.
  std::array<ValueAndIdentifier, S> _values;
};

/**
 * A group presents a set of counters where the first counter is the "group leader".
 * All counters can be started and stopped together, not individually.
 */
class Group
{
public:
  /// Number of maximal members per group.
  constexpr static auto MAX_MEMBERS = 12U;

  /**
   * Creates a copy of the given counter with the same counter configuration.
   *
   * @param other Group to copy from.
   * @return Copied group.
   */
  [[nodiscard]] static Group copy_from_template(const Group& other);

  Group() = default;
  Group(const Group&) = delete;
  Group(Group&&) noexcept = default;

  ~Group() = default;

  Group& operator=(const Group&) = delete;
  Group& operator=(Group&&) noexcept = default;

  /**
   * Adds the given event to the group.
   *
   * @param event_config Event to add.
   */
  void add(CounterConfig event_config);

  /**
   * Opens all counters of the group for event counting, configured by the provided config.
   *
   * @param config Configuration.
   */
  void open(const Config& config);

  /**
   * Opens all counters of the group, configured by the provided config.
   *
   * @param config Configuration.
   * @param has_auxiliary_event True, if the group has an auxiliary event as a first event.
   * @param buffer_pages Number of pages allocated for user-level buffer.
   * @param sample_recording_values Values to record while sampling.
   */
  void open(const Config& config,
            bool has_auxiliary_event,
            std::uint64_t buffer_pages,
            const SampleRecordingValues& sample_recording_values);

  /**
   * Closes all counters of the group.
   */
  void close();

  /**
   * Starts monitoring the counters in the group.
   */
  void start();

  /**
   * Enables the group to start monitoring.
   */
  void enable() const;

  /**
   * Stops monitoring of all counters in the group.
   */
  void stop();

  /**
   * Disables the group to stop monitoring.
   */
  void disable() const;

  /**
   * Reads the counter into the given value.
   *
   * @param values Value to read the counters into.
   * @return True, if reading was successful.
   */
  void read(CounterValues<MAX_MEMBERS>& values);

  /**
   * @return Number of counters in the group.
   */
  [[nodiscard]] std::size_t size() const noexcept { return _members.size(); }

  /**
   * @return True, if the group is empty.
   */
  [[nodiscard]] bool empty() const noexcept { return _members.empty(); }

  /**
   * Reads the result of counter at the given index.
   *
   * @param index Index of the counter to read the result for.
   * @return Result of the counter.
   */
  [[nodiscard]] double get(std::size_t index) const;

  /**
   * Grants access to the counter at the given index.
   *
   * @param index Index of the counter.
   * @return Counter.
   */
  [[nodiscard]] Counter& member(const std::size_t index) noexcept { return _members[index]; }

  /**
   * Grants access to the counter at the given index.
   *
   * @param index Index of the counter.
   * @return Counter.
   */
  [[nodiscard]] const Counter& member(const std::size_t index) const noexcept { return _members[index]; }

  /**
   * @return List of all members in the group.
   */
  [[nodiscard]] std::vector<Counter>& members() noexcept { return _members; }

  /**
   * Calculates the multiplexing factor based on the time the counter was enabled and the time the counter was running.
   *
   * @param time_enabled Time the counter was enabled.
   * @param time_running Time the counter was running.
   * @return Multiplexing factor the result of the events has to be multiplied with.
   */
  [[nodiscard]] static double calculate_multiplexing_factor(
    const CounterValues<MAX_MEMBERS>::time_t time_enabled,
    const CounterValues<MAX_MEMBERS>::time_t time_running) noexcept
  {
    return time_running > 0ULL ? static_cast<double>(time_enabled) / static_cast<double>(time_running) : 1.;
  }

  /**
   * Calculates the multiplexing factor based on the time the counter was enabled and the time the counter was running.
   *
   * @param start Start value of the counter.
   * @param end End value of the counter.
   * @return Multiplexing factor the result of the events has to be multiplied with.
   */
  [[nodiscard]] static double calculate_multiplexing_factor(const CounterValues<MAX_MEMBERS>& start,
                                                            const CounterValues<MAX_MEMBERS>& end) noexcept
  {
    const auto time_enabled = end.time_enabled() - start.time_enabled();
    const auto time_running = end.time_running() - start.time_running();

    return calculate_multiplexing_factor(time_enabled, time_running);
  }

private:
  /// List of all the group members.
  std::vector<Counter> _members;

  /// Start value of the hardware performance counters.
  CounterValues<Group::MAX_MEMBERS> _start_value;

  /// End value of the hardware performance counters.
  CounterValues<Group::MAX_MEMBERS> _end_value;

  /// After stopping the group, we calculate the multiplexing correction once from start- and end-values.
  double _multiplexing_correction{ 1. };
};
}