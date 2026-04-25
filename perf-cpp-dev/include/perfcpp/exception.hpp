#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace perf {

class InvalidConfigAnyCpuCoreAndAnyProcess final : public std::runtime_error
{
public:
  explicit InvalidConfigAnyCpuCoreAndAnyProcess()
    : std::runtime_error("Cannot monitor any process on any CPU core. This configuration is invalid.")
  {
  }
  InvalidConfigAnyCpuCoreAndAnyProcess(const InvalidConfigAnyCpuCoreAndAnyProcess&) = default;
  InvalidConfigAnyCpuCoreAndAnyProcess(InvalidConfigAnyCpuCoreAndAnyProcess&&) noexcept = default;
  InvalidConfigAnyCpuCoreAndAnyProcess& operator=(const InvalidConfigAnyCpuCoreAndAnyProcess&) = default;
  InvalidConfigAnyCpuCoreAndAnyProcess& operator=(InvalidConfigAnyCpuCoreAndAnyProcess&&) noexcept = default;
  ~InvalidConfigAnyCpuCoreAndAnyProcess() override = default;
};

class CannotOpenFileError final : public std::runtime_error
{
public:
  explicit CannotOpenFileError(const std::string_view file_name)
    : std::runtime_error(std::string{ "Cannot open file '" }.append(file_name).append("'."))
  {
  }
  CannotOpenFileError(const CannotOpenFileError&) = default;
  CannotOpenFileError(CannotOpenFileError&&) noexcept = default;
  CannotOpenFileError& operator=(const CannotOpenFileError&) = default;
  CannotOpenFileError& operator=(CannotOpenFileError&&) noexcept = default;
  ~CannotOpenFileError() override = default;
};

class CannotOpenCounterError final : public std::runtime_error
{
public:
  explicit CannotOpenCounterError(const std::int64_t error_code)
    : std::runtime_error(std::string{ "Cannot open perf counter (error no " }
                           .append(std::to_string(error_code))
                           .append("): ")
                           .append(CannotOpenCounterError::create_error_message_from_code(error_code))
                           .append("."))
    , _error_code(error_code)
  {
  }
  CannotOpenCounterError(const CannotOpenCounterError&) = default;
  CannotOpenCounterError(CannotOpenCounterError&&) noexcept = default;
  CannotOpenCounterError& operator=(const CannotOpenCounterError&) = default;
  CannotOpenCounterError& operator=(CannotOpenCounterError&&) noexcept = default;
  ~CannotOpenCounterError() override = default;

  /**
   * @return The error code provided by the perf subsystem.
   */
  [[nodiscard]] std::int64_t error_code() const noexcept { return _error_code; }

private:
  std::int64_t _error_code;
  /**
   * Creates an exception message based on the errno set when accessing the perf subsystem to open an event.
   *
   * @param error_code Error code raised when calling perf_event_open.
   * @return Error message that can be thrown to inform the user.
   */
  [[nodiscard]] static std::string create_error_message_from_code(std::int64_t error_code);
};

class CannotReadCounter final : public std::runtime_error
{
public:
  CannotReadCounter()
    : std::runtime_error(std::string{ "Cannot read from event counter." })
  {
  }
  CannotReadCounter(const CannotReadCounter&) = default;
  CannotReadCounter(CannotReadCounter&&) noexcept = default;
  CannotReadCounter& operator=(const CannotReadCounter&) = default;
  CannotReadCounter& operator=(CannotReadCounter&&) noexcept = default;
  ~CannotReadCounter() override = default;
};

class IoctlError : public std::runtime_error
{
public:
  explicit IoctlError(const std::int64_t error_code, std::string error_message)
    : std::runtime_error(error_message.append(" (error no ")
                           .append(std::to_string(error_code))
                           .append("): ")
                           .append(IoctlError::create_error_message_from_code(error_code))
                           .append("."))
  {
  }

  IoctlError(const IoctlError&) = default;
  IoctlError(IoctlError&&) noexcept = default;
  IoctlError& operator=(const IoctlError&) = default;
  IoctlError& operator=(IoctlError&&) noexcept = default;
  ~IoctlError() override = default;

protected:
  [[nodiscard]] static std::string create_error_message_from_code(std::int64_t error_code);
};

class CannotEnableCounter final : public IoctlError
{
public:
  explicit CannotEnableCounter(const std::int64_t error_code)
    : IoctlError(error_code, "Cannot enable counter")
  {
  }
  CannotEnableCounter(const CannotEnableCounter&) = default;
  CannotEnableCounter(CannotEnableCounter&&) noexcept = default;
  CannotEnableCounter& operator=(const CannotEnableCounter&) = default;
  CannotEnableCounter& operator=(CannotEnableCounter&&) noexcept = default;
  ~CannotEnableCounter() override = default;
};

class CannotDisableCounter final : public IoctlError
{
public:
  explicit CannotDisableCounter(const std::int64_t error_code)
    : IoctlError(error_code, "Cannot disable counter")
  {
  }
  CannotDisableCounter(const CannotDisableCounter&) = default;
  CannotDisableCounter(CannotDisableCounter&&) noexcept = default;
  CannotDisableCounter& operator=(const CannotDisableCounter&) = default;
  CannotDisableCounter& operator=(CannotDisableCounter&&) noexcept = default;
  ~CannotDisableCounter() override = default;
};

class CannotReadCounterId final : public IoctlError
{
public:
  explicit CannotReadCounterId(const std::int64_t error_code)
    : IoctlError(error_code, "Cannot open counter")
  {
  }
  CannotReadCounterId(const CannotReadCounterId&) = default;
  CannotReadCounterId(CannotReadCounterId&&) noexcept = default;
  CannotReadCounterId& operator=(const CannotReadCounterId&) = default;
  CannotReadCounterId& operator=(CannotReadCounterId&&) noexcept = default;
  ~CannotReadCounterId() override = default;
};

class MmapError final : public std::runtime_error
{
public:
  explicit MmapError(const std::int64_t error_code)
    : std::runtime_error(
        std::string{ "Creating buffer via mmap() failed (error no: " }.append(std::to_string(error_code)).append(")."))
  {
  }
  MmapError(const MmapError&) = default;
  MmapError(MmapError&&) noexcept = default;
  MmapError& operator=(const MmapError&) = default;
  MmapError& operator=(MmapError&&) noexcept = default;
  ~MmapError() override = default;
};

class MmapNullError final : public std::runtime_error
{
public:
  MmapNullError()
    : std::runtime_error("Created buffer via mmap() is null.")
  {
  }
  MmapNullError(const MmapNullError&) = default;
  MmapNullError(MmapNullError&&) noexcept = default;
  MmapNullError& operator=(const MmapNullError&) = default;
  MmapNullError& operator=(MmapNullError&&) noexcept = default;
  ~MmapNullError() override = default;
};

class MaxCountersReachedError final : public std::runtime_error
{
public:
  MaxCountersReachedError(const std::uint64_t max_counters, const std::uint64_t max_events_per_counter)
    : std::runtime_error(
        std::string{ "Cannot add more events: reached maximum number of counters and events (" }.append(
          std::to_string(max_counters)
            .append(" counters, ")
            .append(std::to_string(max_events_per_counter))
            .append(" events per counter). Try to increase via perf::Config::max_groups(X) and "
                    "perf::Config::max_counters_per_group(Y).")))
  {
  }

  explicit MaxCountersReachedError(const std::uint64_t max_counters)
    : std::runtime_error(std::string{ "Cannot add more events: reached maximum number of counters (" }.append(
        std::to_string(max_counters).append("). Try to increase via perf::Config::max_groups(X).")))
  {
  }
  MaxCountersReachedError(const MaxCountersReachedError&) = default;
  MaxCountersReachedError(MaxCountersReachedError&&) noexcept = default;
  MaxCountersReachedError& operator=(const MaxCountersReachedError&) = default;
  MaxCountersReachedError& operator=(MaxCountersReachedError&&) noexcept = default;
  ~MaxCountersReachedError() override = default;
};

class MaxPhysicalCountersReachedError final : public std::runtime_error
{
public:
  explicit MaxPhysicalCountersReachedError(const std::uint64_t num_physical_counters)
    : std::runtime_error(
        std::string{ "Cannot add more events: reached maximum number of physical performance counters (" }.append(
          std::to_string(num_physical_counters)
            .append("). Try to increase via perf::Config::num_physical_counters(X).")))
  {
  }
  MaxPhysicalCountersReachedError(const MaxPhysicalCountersReachedError&) = default;
  MaxPhysicalCountersReachedError(MaxPhysicalCountersReachedError&&) noexcept = default;
  MaxPhysicalCountersReachedError& operator=(const MaxPhysicalCountersReachedError&) = default;
  MaxPhysicalCountersReachedError& operator=(MaxPhysicalCountersReachedError&&) noexcept = default;
  ~MaxPhysicalCountersReachedError() override = default;
};

/// Keep MaxGroupsReachedError for backward compatibility (changed in v0.13.1).
using MaxGroupsReachedError = MaxPhysicalCountersReachedError;

class CannotAddEventToSingleGroupError final : public std::runtime_error
{
public:
  explicit CannotAddEventToSingleGroupError(const std::uint64_t num_events_per_physical_counter)
    : std::runtime_error(std::string{ "Cannot add more than " }
                           .append(std::to_string(num_events_per_physical_counter))
                           .append(" events to a single physical counter. Try to increase via "
                                   "perf::Config::num_events_per_physical_counter(X)."))
  {
  }
  CannotAddEventToSingleGroupError(const CannotAddEventToSingleGroupError&) = default;
  CannotAddEventToSingleGroupError(CannotAddEventToSingleGroupError&&) noexcept = default;
  CannotAddEventToSingleGroupError& operator=(const CannotAddEventToSingleGroupError&) = default;
  CannotAddEventToSingleGroupError& operator=(CannotAddEventToSingleGroupError&&) noexcept = default;
  ~CannotAddEventToSingleGroupError() override = default;
};

class CannotAddEventWhenOpenedError final : public std::runtime_error
{
public:
  CannotAddEventWhenOpenedError()
    : std::runtime_error("Cannot add events to an opened EventCounter. Please call close() first.")
  {
  }
  CannotAddEventWhenOpenedError(const CannotAddEventWhenOpenedError&) = default;
  CannotAddEventWhenOpenedError(CannotAddEventWhenOpenedError&&) noexcept = default;
  CannotAddEventWhenOpenedError& operator=(const CannotAddEventWhenOpenedError&) = default;
  CannotAddEventWhenOpenedError& operator=(CannotAddEventWhenOpenedError&&) noexcept = default;
  ~CannotAddEventWhenOpenedError() override = default;
};

class CannotFindEventForMetricError final : public std::runtime_error
{
public:
  CannotFindEventForMetricError(const std::string_view event_name, const std::string_view metric_name)
    : std::runtime_error(std::string{ "Cannot find an event with name '" }
                           .append(event_name)
                           .append("' for metric '")
                           .append(metric_name)
                           .append("'."))
  {
  }
  CannotFindEventForMetricError(const CannotFindEventForMetricError&) = default;
  CannotFindEventForMetricError(CannotFindEventForMetricError&&) noexcept = default;
  CannotFindEventForMetricError& operator=(const CannotFindEventForMetricError&) = default;
  CannotFindEventForMetricError& operator=(CannotFindEventForMetricError&&) noexcept = default;
  ~CannotFindEventForMetricError() override = default;
};

class CannotFindEventOrMetricError final : public std::runtime_error
{
public:
  explicit CannotFindEventOrMetricError(const std::string& event_name)
    : std::runtime_error(std::string{ "Cannot find an event or metric with name '" }.append(event_name).append("'."))
  {
  }
  CannotFindEventOrMetricError(const CannotFindEventOrMetricError&) = default;
  CannotFindEventOrMetricError(CannotFindEventOrMetricError&&) noexcept = default;
  CannotFindEventOrMetricError& operator=(const CannotFindEventOrMetricError&) = default;
  CannotFindEventOrMetricError& operator=(CannotFindEventOrMetricError&&) noexcept = default;
  ~CannotFindEventOrMetricError() override = default;
};

class CannotEvaluateMetricsBecauseOfCycleError final : public std::runtime_error
{
public:
  explicit CannotEvaluateMetricsBecauseOfCycleError()
    : std::runtime_error(std::string{ "Cannot evaluate metrics because they are mutually (cyclically) dependent. " })
  {
  }
  CannotEvaluateMetricsBecauseOfCycleError(const CannotEvaluateMetricsBecauseOfCycleError&) = default;
  CannotEvaluateMetricsBecauseOfCycleError(CannotEvaluateMetricsBecauseOfCycleError&&) noexcept = default;
  CannotEvaluateMetricsBecauseOfCycleError& operator=(const CannotEvaluateMetricsBecauseOfCycleError&) = default;
  CannotEvaluateMetricsBecauseOfCycleError& operator=(CannotEvaluateMetricsBecauseOfCycleError&&) noexcept = default;
  ~CannotEvaluateMetricsBecauseOfCycleError() override = default;
};

class CannotFindEventError final : public std::runtime_error
{
public:
  explicit CannotFindEventError(const std::string& event_name)
    : CannotFindEventError(std::string_view{ event_name })
  {
  }

  explicit CannotFindEventError(const std::string_view event_name)
    : std::runtime_error(std::string{ "Cannot find an event with name '" }.append(event_name).append("'."))
  {
  }

  CannotFindEventError(const std::string_view pmu_name, const std::string_view event_name)
    : std::runtime_error(std::string{ "Cannot find an event with name '" }
                           .append(event_name)
                           .append("' for the PMU '")
                           .append(pmu_name)
                           .append("'."))
  {
  }

  CannotFindEventError(const CannotFindEventError&) = default;
  CannotFindEventError(CannotFindEventError&&) noexcept = default;
  CannotFindEventError& operator=(const CannotFindEventError&) = default;
  CannotFindEventError& operator=(CannotFindEventError&&) noexcept = default;
  ~CannotFindEventError() override = default;
};

class CannotChangeTriggerWhenSamplerOpenedError final : public std::runtime_error
{
public:
  CannotChangeTriggerWhenSamplerOpenedError()
    : std::runtime_error(
        "The Sampler was already opened. Cannot modify triggers after opening. Please create a new Sampler.")
  {
  }
  CannotChangeTriggerWhenSamplerOpenedError(const CannotChangeTriggerWhenSamplerOpenedError&) = default;
  CannotChangeTriggerWhenSamplerOpenedError(CannotChangeTriggerWhenSamplerOpenedError&&) noexcept = default;
  CannotChangeTriggerWhenSamplerOpenedError& operator=(const CannotChangeTriggerWhenSamplerOpenedError&) = default;
  CannotChangeTriggerWhenSamplerOpenedError& operator=(CannotChangeTriggerWhenSamplerOpenedError&&) noexcept = default;
  ~CannotChangeTriggerWhenSamplerOpenedError() override = default;
};

class MetricNotSupportedAsSamplingTriggerError final : public std::runtime_error
{
public:
  explicit MetricNotSupportedAsSamplingTriggerError(const std::string& metric_name)
    : std::runtime_error(std::string{ "The event '" }
                           .append(metric_name)
                           .append("' appears to be a metric. Metrics are not supported as sampling triggers."))
  {
  }
  MetricNotSupportedAsSamplingTriggerError(const MetricNotSupportedAsSamplingTriggerError&) = default;
  MetricNotSupportedAsSamplingTriggerError(MetricNotSupportedAsSamplingTriggerError&&) noexcept = default;
  MetricNotSupportedAsSamplingTriggerError& operator=(const MetricNotSupportedAsSamplingTriggerError&) = default;
  MetricNotSupportedAsSamplingTriggerError& operator=(MetricNotSupportedAsSamplingTriggerError&&) noexcept = default;
  ~MetricNotSupportedAsSamplingTriggerError() override = default;
};

class TriggerIsAmbiguousError final : public std::runtime_error
{
public:
  explicit TriggerIsAmbiguousError(const std::string& metric_name)
    : std::runtime_error(
        std::string{ "The event '" }.append(metric_name).append("' is ambiguous. Please specify the trigger."))
  {
  }
  TriggerIsAmbiguousError(const TriggerIsAmbiguousError&) = default;
  TriggerIsAmbiguousError(TriggerIsAmbiguousError&&) noexcept = default;
  TriggerIsAmbiguousError& operator=(const TriggerIsAmbiguousError&) = default;
  TriggerIsAmbiguousError& operator=(TriggerIsAmbiguousError&&) noexcept = default;
  ~TriggerIsAmbiguousError() override = default;
};

class MetricNotSupportedAsLiveEventError final : public std::runtime_error
{
public:
  explicit MetricNotSupportedAsLiveEventError(const std::string& metric_name)
    : std::runtime_error(std::string{ "The event '" }
                           .append(metric_name)
                           .append("' appears to be a metric. Metrics are not supported as live events."))
  {
  }
  MetricNotSupportedAsLiveEventError(const MetricNotSupportedAsLiveEventError&) = default;
  MetricNotSupportedAsLiveEventError(MetricNotSupportedAsLiveEventError&&) noexcept = default;
  MetricNotSupportedAsLiveEventError& operator=(const MetricNotSupportedAsLiveEventError&) = default;
  MetricNotSupportedAsLiveEventError& operator=(MetricNotSupportedAsLiveEventError&&) noexcept = default;
  ~MetricNotSupportedAsLiveEventError() override = default;
};

class TimeEventNotSupportedAsLiveEventError final : public std::runtime_error
{
public:
  explicit TimeEventNotSupportedAsLiveEventError(const std::string& time_event)
    : std::runtime_error(std::string{ "The event '" }
                           .append(time_event)
                           .append("' appears to be a time event. Time events are not supported as live events."))
  {
  }
  TimeEventNotSupportedAsLiveEventError(const TimeEventNotSupportedAsLiveEventError&) = default;
  TimeEventNotSupportedAsLiveEventError(TimeEventNotSupportedAsLiveEventError&&) noexcept = default;
  TimeEventNotSupportedAsLiveEventError& operator=(const TimeEventNotSupportedAsLiveEventError&) = default;
  TimeEventNotSupportedAsLiveEventError& operator=(TimeEventNotSupportedAsLiveEventError&&) noexcept = default;
  ~TimeEventNotSupportedAsLiveEventError() override = default;
};

class LiveEventCounterResultMismatchError final : public std::runtime_error
{
public:
  explicit LiveEventCounterResultMismatchError(const std::size_t live_event_counter_size, const std::size_t result_size)
    : std::runtime_error(std::string{ "The number of live event counters (" }
                           .append(std::to_string(live_event_counter_size))
                           .append(" does not match the length of results list (")
                           .append(std::to_string(result_size))
                           .append(")."))
  {
  }
  LiveEventCounterResultMismatchError(const LiveEventCounterResultMismatchError&) = default;
  LiveEventCounterResultMismatchError(LiveEventCounterResultMismatchError&&) noexcept = default;
  LiveEventCounterResultMismatchError& operator=(const LiveEventCounterResultMismatchError&) = default;
  LiveEventCounterResultMismatchError& operator=(LiveEventCounterResultMismatchError&&) noexcept = default;
  ~LiveEventCounterResultMismatchError() override = default;
};

class LiveEventCounterOutOfBoundsAccessError final : public std::runtime_error
{
public:
  explicit LiveEventCounterOutOfBoundsAccessError(const std::size_t live_event_counter_size,
                                                  const std::size_t counter_index)
    : std::runtime_error(std::string{ "The accessed live counter (" }
                           .append(std::to_string(counter_index))
                           .append(" is larger then the number if configured live counters (")
                           .append(std::to_string(live_event_counter_size))
                           .append(")."))
  {
  }
  LiveEventCounterOutOfBoundsAccessError(const LiveEventCounterOutOfBoundsAccessError&) = default;
  LiveEventCounterOutOfBoundsAccessError(LiveEventCounterOutOfBoundsAccessError&&) noexcept = default;
  LiveEventCounterOutOfBoundsAccessError& operator=(const LiveEventCounterOutOfBoundsAccessError&) = default;
  LiveEventCounterOutOfBoundsAccessError& operator=(LiveEventCounterOutOfBoundsAccessError&&) noexcept = default;
  ~LiveEventCounterOutOfBoundsAccessError() override = default;
};

class TimeEventNotSupportedForSamplingError final : public std::runtime_error
{
public:
  explicit TimeEventNotSupportedForSamplingError(const std::string_view event_name)
    : std::runtime_error(std::string{ "The event '" }
                           .append(event_name)
                           .append("' appears to be a time event. Time events are not supported for sampling."))
  {
  }
  TimeEventNotSupportedForSamplingError(const TimeEventNotSupportedForSamplingError&) = default;
  TimeEventNotSupportedForSamplingError(TimeEventNotSupportedForSamplingError&&) noexcept = default;
  TimeEventNotSupportedForSamplingError& operator=(const TimeEventNotSupportedForSamplingError&) = default;
  TimeEventNotSupportedForSamplingError& operator=(TimeEventNotSupportedForSamplingError&&) noexcept = default;
  ~TimeEventNotSupportedForSamplingError() override = default;
};

class CannotStartEmptyGroupError final : public std::runtime_error
{
public:
  CannotStartEmptyGroupError()
    : std::runtime_error("Cannot start an empty group. Please add at least one counter.")
  {
  }
  CannotStartEmptyGroupError(const CannotStartEmptyGroupError&) = default;
  CannotStartEmptyGroupError(CannotStartEmptyGroupError&&) noexcept = default;
  CannotStartEmptyGroupError& operator=(const CannotStartEmptyGroupError&) = default;
  CannotStartEmptyGroupError& operator=(CannotStartEmptyGroupError&&) noexcept = default;
  ~CannotStartEmptyGroupError() override = default;
};

class CannotStartEmptySamplerError final : public std::runtime_error
{
public:
  CannotStartEmptySamplerError()
    : std::runtime_error(
        "Cannot start sampling without any trigger event. Please specify at least one trigger via Sampler::trigger().")
  {
  }
  CannotStartEmptySamplerError(const CannotStartEmptySamplerError&) = default;
  CannotStartEmptySamplerError(CannotStartEmptySamplerError&&) noexcept = default;
  CannotStartEmptySamplerError& operator=(const CannotStartEmptySamplerError&) = default;
  CannotStartEmptySamplerError& operator=(CannotStartEmptySamplerError&&) noexcept = default;
  ~CannotStartEmptySamplerError() override = default;
};

class SamplingFeatureIsNotSupported final : public std::runtime_error
{
public:
  SamplingFeatureIsNotSupported(const std::string_view feature_name, const std::string_view linux_kernel_version)
    : std::runtime_error(std::string{ "Sampling " }
                           .append(feature_name)
                           .append(" is only supported from Linux ")
                           .append(linux_kernel_version)
                           .append("."))
  {
  }

  SamplingFeatureIsNotSupported(const SamplingFeatureIsNotSupported&) = default;
  SamplingFeatureIsNotSupported(SamplingFeatureIsNotSupported&&) noexcept = default;
  SamplingFeatureIsNotSupported& operator=(const SamplingFeatureIsNotSupported&) = default;
  SamplingFeatureIsNotSupported& operator=(SamplingFeatureIsNotSupported&&) noexcept = default;
  ~SamplingFeatureIsNotSupported() override = default;
};

class AuxiliaryEventForSamplingNotFoundError final : public std::runtime_error
{
public:
  AuxiliaryEventForSamplingNotFoundError()
    : std::runtime_error("The underlying hardware requires an auxiliary counter for sampling memory loads but the "
                         "auxiliary event cannot be found.")
  {
  }
  AuxiliaryEventForSamplingNotFoundError(const AuxiliaryEventForSamplingNotFoundError&) = default;
  AuxiliaryEventForSamplingNotFoundError(AuxiliaryEventForSamplingNotFoundError&&) noexcept = default;
  AuxiliaryEventForSamplingNotFoundError& operator=(const AuxiliaryEventForSamplingNotFoundError&) = default;
  AuxiliaryEventForSamplingNotFoundError& operator=(AuxiliaryEventForSamplingNotFoundError&&) noexcept = default;
  ~AuxiliaryEventForSamplingNotFoundError() override = default;
};

class DataTypeAlreadyRegisteredError final : public std::runtime_error
{
public:
  explicit DataTypeAlreadyRegisteredError(const std::string_view data_type_name)
    : std::runtime_error(std::string{ "The DataType '" }
                           .append(data_type_name)
                           .append("' is already registered and cannot be registered twice."))
  {
  }
  DataTypeAlreadyRegisteredError(const DataTypeAlreadyRegisteredError&) = default;
  DataTypeAlreadyRegisteredError(DataTypeAlreadyRegisteredError&&) noexcept = default;
  DataTypeAlreadyRegisteredError& operator=(const DataTypeAlreadyRegisteredError&) = default;
  DataTypeAlreadyRegisteredError& operator=(DataTypeAlreadyRegisteredError&&) noexcept = default;
  ~DataTypeAlreadyRegisteredError() override = default;
};

class DataTypeNotRegisteredError final : public std::runtime_error
{
public:
  explicit DataTypeNotRegisteredError(const std::string_view data_type_name)
    : std::runtime_error(std::string{ "The DataType '" }.append(data_type_name).append("' is was not found."))
  {
  }
  DataTypeNotRegisteredError(const DataTypeNotRegisteredError&) = default;
  DataTypeNotRegisteredError(DataTypeNotRegisteredError&&) noexcept = default;
  DataTypeNotRegisteredError& operator=(const DataTypeNotRegisteredError&) = default;
  DataTypeNotRegisteredError& operator=(DataTypeNotRegisteredError&&) noexcept = default;
  ~DataTypeNotRegisteredError() override = default;
};

class CannotParseMetricExpressionError final : public std::runtime_error
{
public:
  explicit CannotParseMetricExpressionError(const std::string_view input)
    : std::runtime_error(std::string{ "Cannot parse expression from '" }.append(input).append("'."))
  {
  }

  CannotParseMetricExpressionError(const std::string_view input, const std::string_view reason)
    : std::runtime_error(
        std::string{ "Cannot parse expression from '" }.append(input).append("': ").append(reason).append("."))
  {
  }

  CannotParseMetricExpressionError(const CannotParseMetricExpressionError&) = default;
  CannotParseMetricExpressionError(CannotParseMetricExpressionError&&) noexcept = default;
  CannotParseMetricExpressionError& operator=(const CannotParseMetricExpressionError&) = default;
  CannotParseMetricExpressionError& operator=(CannotParseMetricExpressionError&&) noexcept = default;
  ~CannotParseMetricExpressionError() override = default;
};

class CannotParseMetricExpressionUnknownFunctionError final : public std::runtime_error
{
public:
  explicit CannotParseMetricExpressionUnknownFunctionError(const std::string_view input,
                                                           const std::string_view function_name)
    : std::runtime_error(std::string{ "Cannot parse expression. Unknown function '" }
                           .append(function_name)
                           .append("' in expression '")
                           .append(input)
                           .append("'."))
  {
  }

  CannotParseMetricExpressionUnknownFunctionError(const CannotParseMetricExpressionUnknownFunctionError&) = default;
  CannotParseMetricExpressionUnknownFunctionError(CannotParseMetricExpressionUnknownFunctionError&&) noexcept = default;
  CannotParseMetricExpressionUnknownFunctionError& operator=(const CannotParseMetricExpressionUnknownFunctionError&) =
    default;
  CannotParseMetricExpressionUnknownFunctionError& operator=(
    CannotParseMetricExpressionUnknownFunctionError&&) noexcept = default;
  ~CannotParseMetricExpressionUnknownFunctionError() override = default;
};

class CannotParseMetricExpressionUnexpectedFunctionArgumentsError final : public std::runtime_error
{
public:
  explicit CannotParseMetricExpressionUnexpectedFunctionArgumentsError(const std::string_view input,
                                                                       const std::string_view function_name,
                                                                       const std::size_t expected_arguments,
                                                                       const std::size_t arguments)
    : std::runtime_error(std::string{ "Cannot parse expression. Function '" }
                           .append(function_name)
                           .append("' takes ")
                           .append(std::to_string(expected_arguments))
                           .append(" arguments, got ")
                           .append(std::to_string(arguments))
                           .append(" in expression '")
                           .append(input)
                           .append("'."))
  {
  }

  CannotParseMetricExpressionUnexpectedFunctionArgumentsError(
    const CannotParseMetricExpressionUnexpectedFunctionArgumentsError&) = default;
  CannotParseMetricExpressionUnexpectedFunctionArgumentsError(
    CannotParseMetricExpressionUnexpectedFunctionArgumentsError&&) noexcept = default;
  CannotParseMetricExpressionUnexpectedFunctionArgumentsError& operator=(
    const CannotParseMetricExpressionUnexpectedFunctionArgumentsError&) = default;
  CannotParseMetricExpressionUnexpectedFunctionArgumentsError& operator=(
    CannotParseMetricExpressionUnexpectedFunctionArgumentsError&&) noexcept = default;
  ~CannotParseMetricExpressionUnexpectedFunctionArgumentsError() override = default;
};

class CannotCreateEventFileDescriptor final : public std::runtime_error
{
public:
  CannotCreateEventFileDescriptor()
    : std::runtime_error(std::string{ "Cannot create eventfd for file descriptor." })
  {
  }

  CannotCreateEventFileDescriptor(const CannotCreateEventFileDescriptor&) = default;
  CannotCreateEventFileDescriptor(CannotCreateEventFileDescriptor&&) noexcept = default;
  CannotCreateEventFileDescriptor& operator=(const CannotCreateEventFileDescriptor&) = default;
  CannotCreateEventFileDescriptor& operator=(CannotCreateEventFileDescriptor&&) noexcept = default;
  ~CannotCreateEventFileDescriptor() override = default;
};

class CannotAddHeaderToTable final : public std::runtime_error
{
public:
  CannotAddHeaderToTable(const std::uint64_t columns, const std::uint64_t expected_columns)
    : std::runtime_error{ std::string{ "Header does not match the columns. Provided columns is " }
                            .append(std::to_string(columns))
                            .append(", expected is ")
                            .append(std::to_string(expected_columns))
                            .append(".") }
  {
  }
  CannotAddHeaderToTable(const CannotAddHeaderToTable&) = default;
  CannotAddHeaderToTable(CannotAddHeaderToTable&&) noexcept = default;
  CannotAddHeaderToTable& operator=(const CannotAddHeaderToTable&) = default;
  CannotAddHeaderToTable& operator=(CannotAddHeaderToTable&&) noexcept = default;
  ~CannotAddHeaderToTable() override = default;
};

class CannotAddRowToTable final : public std::runtime_error
{
public:
  CannotAddRowToTable(const std::uint64_t columns, const std::uint64_t expected_columns)
    : std::runtime_error{ std::string{ "Row does not match the columns. Provided columns is " }
                            .append(std::to_string(columns))
                            .append(", expected is ")
                            .append(std::to_string(expected_columns))
                            .append(".") }
  {
  }
  CannotAddRowToTable(const CannotAddRowToTable&) = default;
  CannotAddRowToTable(CannotAddRowToTable&&) noexcept = default;
  CannotAddRowToTable& operator=(const CannotAddRowToTable&) = default;
  CannotAddRowToTable& operator=(CannotAddRowToTable&&) noexcept = default;
  ~CannotAddRowToTable() override = default;
};

class CannotReadSymbolsForModule final : public std::runtime_error
{
public:
  CannotReadSymbolsForModule(const std::string_view name, const std::string_view path)
    : std::runtime_error{
      std::string{ "Cannot read symbols for module " }.append(name).append(" from path").append(path).append(".")
    }
  {
  }
  CannotReadSymbolsForModule(const CannotReadSymbolsForModule&) = default;
  CannotReadSymbolsForModule(CannotReadSymbolsForModule&&) noexcept = default;
  CannotReadSymbolsForModule& operator=(const CannotReadSymbolsForModule&) = default;
  CannotReadSymbolsForModule& operator=(CannotReadSymbolsForModule&&) noexcept = default;
  ~CannotReadSymbolsForModule() override = default;
};

class CannotReadFstatForModule final : public std::runtime_error
{
public:
  CannotReadFstatForModule(const std::string_view name, const std::string_view path)
    : std::runtime_error{
      std::string{ "Cannot read fstat for module " }.append(name).append(" from path").append(path).append(".")
    }
  {
  }
  CannotReadFstatForModule(const CannotReadFstatForModule&) = default;
  CannotReadFstatForModule(CannotReadFstatForModule&&) noexcept = default;
  CannotReadFstatForModule& operator=(const CannotReadFstatForModule&) = default;
  CannotReadFstatForModule& operator=(CannotReadFstatForModule&&) noexcept = default;
  ~CannotReadFstatForModule() override = default;
};

class CannotReadElfForModule final : public std::runtime_error
{
public:
  CannotReadElfForModule(const std::string_view name, const std::string_view path)
    : std::runtime_error{
      std::string{ "Cannot read ELF data for module " }.append(name).append(" from path").append(path).append(".")
    }
  {
  }
  CannotReadElfForModule(const CannotReadElfForModule&) = default;
  CannotReadElfForModule(CannotReadElfForModule&&) noexcept = default;
  CannotReadElfForModule& operator=(const CannotReadElfForModule&) = default;
  CannotReadElfForModule& operator=(CannotReadElfForModule&&) noexcept = default;
  ~CannotReadElfForModule() override = default;
};

class CannotVerifyElfMagicForModule final : public std::runtime_error
{
public:
  CannotVerifyElfMagicForModule(const std::string_view name, const std::string_view path)
    : std::runtime_error{
      std::string{ "Cannot verify ELF magic for module " }.append(name).append(" from path").append(path).append(".")
    }
  {
  }
  CannotVerifyElfMagicForModule(const CannotVerifyElfMagicForModule&) = default;
  CannotVerifyElfMagicForModule(CannotVerifyElfMagicForModule&&) noexcept = default;
  CannotVerifyElfMagicForModule& operator=(const CannotVerifyElfMagicForModule&) = default;
  CannotVerifyElfMagicForModule& operator=(CannotVerifyElfMagicForModule&&) noexcept = default;
  ~CannotVerifyElfMagicForModule() override = default;
};

class CannotReadMaxClockFrequency final : public std::runtime_error
{
public:
  CannotReadMaxClockFrequency()
    : std::runtime_error{ std::string{ "Cannot read max CPU clock frequency." } }
  {
  }
  CannotReadMaxClockFrequency(const CannotReadMaxClockFrequency&) = default;
  CannotReadMaxClockFrequency(CannotReadMaxClockFrequency&&) noexcept = default;
  CannotReadMaxClockFrequency& operator=(const CannotReadMaxClockFrequency&) = default;
  CannotReadMaxClockFrequency& operator=(CannotReadMaxClockFrequency&&) noexcept = default;
  ~CannotReadMaxClockFrequency() override = default;
};

class EventRequiresSpecificVendorError final : public std::runtime_error
{
public:
  EventRequiresSpecificVendorError(const std::string_view vendor_name, const std::string_view event_name)
    : std::runtime_error{
      std::string{ "The event '" }.append(event_name).append("' requires ").append(vendor_name).append(" hardware.")
    }
  {
  }
  EventRequiresSpecificVendorError(const EventRequiresSpecificVendorError&) = default;
  EventRequiresSpecificVendorError(EventRequiresSpecificVendorError&&) noexcept = default;
  EventRequiresSpecificVendorError& operator=(const EventRequiresSpecificVendorError&) = default;
  EventRequiresSpecificVendorError& operator=(EventRequiresSpecificVendorError&&) noexcept = default;
  ~EventRequiresSpecificVendorError() override = default;
};

class EventDoesNotSupportIBSFeatureError final : public std::runtime_error
{
public:
  EventDoesNotSupportIBSFeatureError(const std::string_view ibs_event, const std::string_view feature)
    : std::runtime_error{ std::string{ "The underlying IBS counter '" }
                            .append(ibs_event)
                            .append("' does not support ")
                            .append(feature)
                            .append(".") }
  {
  }
  EventDoesNotSupportIBSFeatureError(const EventDoesNotSupportIBSFeatureError&) = default;
  EventDoesNotSupportIBSFeatureError(EventDoesNotSupportIBSFeatureError&&) noexcept = default;
  EventDoesNotSupportIBSFeatureError& operator=(const EventDoesNotSupportIBSFeatureError&) = default;
  EventDoesNotSupportIBSFeatureError& operator=(EventDoesNotSupportIBSFeatureError&&) noexcept = default;
  ~EventDoesNotSupportIBSFeatureError() override = default;
};

class ThreadIdOutOfBoundsError final : public std::runtime_error
{
public:
  explicit ThreadIdOutOfBoundsError(const std::size_t thread_id)
    : std::runtime_error{ std::string{ "Thread ID '" }.append(std::to_string(thread_id)).append("' is out of bounds.") }
  {
  }
  ThreadIdOutOfBoundsError(const ThreadIdOutOfBoundsError&) = default;
  ThreadIdOutOfBoundsError(ThreadIdOutOfBoundsError&&) noexcept = default;
  ThreadIdOutOfBoundsError& operator=(const ThreadIdOutOfBoundsError&) = default;
  ThreadIdOutOfBoundsError& operator=(ThreadIdOutOfBoundsError&&) noexcept = default;
  ~ThreadIdOutOfBoundsError() override = default;
};

}