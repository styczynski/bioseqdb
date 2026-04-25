#pragma once

#include <string>
#include <utility>
#include <vector>

namespace perf {
class CounterDefinition;
class EventProvider
{
public:
  EventProvider() = default;
  EventProvider(const EventProvider&) = default;
  EventProvider(EventProvider&&) noexcept = default;
  EventProvider& operator=(const EventProvider&) = default;
  EventProvider& operator=(EventProvider&&) noexcept = default;
  virtual ~EventProvider() noexcept = default;

  /**
   * Adds events to the counter definition.
   * @param counter_definition Counter definition to add events to.
   */
  virtual void add_events(CounterDefinition& counter_definition) = 0;
};

/**
 * Event provider to add events defined by the perf subsystem.
 */
class PerfSubsystemEventProvider final : public EventProvider
{
public:
  PerfSubsystemEventProvider() = default;
  PerfSubsystemEventProvider(const PerfSubsystemEventProvider&) = default;
  PerfSubsystemEventProvider(PerfSubsystemEventProvider&&) noexcept = default;
  PerfSubsystemEventProvider& operator=(const PerfSubsystemEventProvider&) = default;
  PerfSubsystemEventProvider& operator=(PerfSubsystemEventProvider&&) noexcept = default;
  ~PerfSubsystemEventProvider() noexcept override = default;

  /**
   * Adds all general purpose events provided by the perf subsystem.
   * @param counter_definition Counter definition to add perf subsystem GP events to.
   */
  void add_events(CounterDefinition& counter_definition) override;
};

/**
 * Event provider to add virtual time events.
 */
class TimeEventProvider final : public EventProvider
{
public:
  TimeEventProvider() = default;
  TimeEventProvider(const TimeEventProvider&) = default;
  TimeEventProvider(TimeEventProvider&&) noexcept = default;
  TimeEventProvider& operator=(const TimeEventProvider&) = default;
  TimeEventProvider& operator=(TimeEventProvider&&) noexcept = default;
  ~TimeEventProvider() noexcept override = default;

  /**
   * Adds virtual time events.
   * @param counter_definition Counter definition to add events to.
   */
  void add_events(CounterDefinition& counter_definition) override;
};

/**
 * Event provider to add metrics.
 */
class MetricEventProvider final : public EventProvider
{
public:
  MetricEventProvider() = default;
  MetricEventProvider(const MetricEventProvider&) = default;
  MetricEventProvider(MetricEventProvider&&) noexcept = default;
  MetricEventProvider& operator=(const MetricEventProvider&) = default;
  MetricEventProvider& operator=(MetricEventProvider&&) noexcept = default;
  ~MetricEventProvider() noexcept override = default;

  /**
   * Adds metrics.
   * @param counter_definition Counter definition to add events to.
   */
  void add_events(CounterDefinition& counter_definition) override;
};

/**
 * Event provider to add events defined by the system using the file system.
 */
class SystemSpecificEventProvider final : public EventProvider
{
public:
  SystemSpecificEventProvider() = default;
  SystemSpecificEventProvider(const SystemSpecificEventProvider&) = default;
  SystemSpecificEventProvider(SystemSpecificEventProvider&&) noexcept = default;
  SystemSpecificEventProvider& operator=(const SystemSpecificEventProvider&) = default;
  SystemSpecificEventProvider& operator=(SystemSpecificEventProvider&&) noexcept = default;
  ~SystemSpecificEventProvider() noexcept override = default;

  /**
   * Adds events provided by the system.
   * @param counter_definition Counter definition to add events to.
   */
  void add_events(CounterDefinition& counter_definition) override;

private:
  /**
   * Reads all events provided by the PMU at the provided path and adds them to the provided counter definition.
   *
   * @param counter_definition Counter definition to add events to.
   * @param pmu_name Name of the PMU.
   * @param path Path in the filesystem, containing event, type, and format files.
   */
  static void add_events(CounterDefinition& counter_definition, const std::string& pmu_name, const std::string& path);

  /**
   * Tries to identify more PMUs by searching "/sys/bus/event_source/devices/" for subfolders with the given pattern.
   * The identified PMUs are appended to the given list of PMUs.
   *
   * @param regex_pattern Pattern the subfolders must match.
   * @param performance_monitoring_units List of PMUs.
   * @return A list of pairs (path, pmu name) where "pmu name" contains "-" instead of "_".
   */
  static void detect_performance_monitoring_units(
    std::string&& regex_pattern,
    std::vector<std::pair<std::string, std::string>>& performance_monitoring_units);
};

/**
 * Event provider to add events defined by AMD's Instruction Based Sampling PMU.
 */
class AMDIbsEventProvider final : public EventProvider
{
public:
  AMDIbsEventProvider() = default;
  AMDIbsEventProvider(const AMDIbsEventProvider&) = default;
  AMDIbsEventProvider(AMDIbsEventProvider&&) noexcept = default;
  AMDIbsEventProvider& operator=(const AMDIbsEventProvider&) = default;
  AMDIbsEventProvider& operator=(AMDIbsEventProvider&&) noexcept = default;
  ~AMDIbsEventProvider() noexcept override = default;

  /**
   * Adds events provided by AMD's Instruction Based Sampling PMU.
   * @param counter_definition Counter definition to add events to.
   */
  void add_events(CounterDefinition& counter_definition) override;

private:
  /**
   * Adds events from the IBS Op PMU.
   * @param counter_definition Counter definition to add events to.
   */
  static void add_fetch_events(CounterDefinition& counter_definition);

  /**
   * Adds events from the IBS Op PMU.
   * @param counter_definition Counter definition to add events to.
   */
  static void add_op_events(CounterDefinition& counter_definition);
};

/**
 * Event provider to add events defined by an external (CSV) file.
 */
class CsvFileEventProvider final : public EventProvider
{
public:
  explicit CsvFileEventProvider(const std::string& file_name) noexcept
    : _file_name(file_name)
  {
  }
  CsvFileEventProvider(const CsvFileEventProvider&) = default;
  CsvFileEventProvider(CsvFileEventProvider&&) noexcept = default;
  CsvFileEventProvider& operator=(const CsvFileEventProvider&) = delete;
  CsvFileEventProvider& operator=(CsvFileEventProvider&&) noexcept = delete;
  ~CsvFileEventProvider() noexcept override = default;

  /**
   * Adds events provided by an external file.
   * @param counter_definition Counter definition to add events to.
   */
  void add_events(CounterDefinition& counter_definition) override;

private:
  const std::string& _file_name;
};

#ifdef PERFCPP_HAS_PROCESSOR_SPECIFIC_EVENTS
/**
 * Event provider to processor-specific events.
 */
class ProcessorSpecificEventProvider final : public EventProvider
{
public:
  ProcessorSpecificEventProvider() = default;
  ProcessorSpecificEventProvider(const ProcessorSpecificEventProvider&) = default;
  ProcessorSpecificEventProvider(ProcessorSpecificEventProvider&&) noexcept = default;
  ProcessorSpecificEventProvider& operator=(const ProcessorSpecificEventProvider&) = default;
  ProcessorSpecificEventProvider& operator=(ProcessorSpecificEventProvider&&) noexcept = default;
  ~ProcessorSpecificEventProvider() noexcept override = default;

  /**
   * Adds processor specific events. The implementation will be generated by the CMake file.
   * @param counter_definition Counter definition to add events to.
   */
  void add_events(CounterDefinition& counter_definition) override;
};
#endif

}