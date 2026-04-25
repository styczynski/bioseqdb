#pragma once

#include <functional>
#include <optional>
#include <perfcpp/counter/group.hpp>
#include <perfcpp/counter/requested_event.hpp>
#include <perfcpp/counter_definition.hpp>
#include <perfcpp/feature.h>
#include <perfcpp/sample/config.hpp>
#include <perfcpp/sample/recording_values.hpp>
#include <perfcpp/sample/result.hpp>
#include <perfcpp/sample/sample.hpp>
#include <perfcpp/sample/trigger.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace perf {
class MultiSamplerBase;
class MultiThreadSampler;
class MultiCoreSampler;
class Sampler
{
  friend MultiSamplerBase;

public:
  /**
   * Represents a trigger condition for initiating a sampling event.
   */
  class Trigger
  {
  public:
    using trigger_t = std::variant<std::string, Cycles, MemoryLoads, MemoryLoadsAux, MemoryStores, IbsFetch, IbsOp>;

    explicit Trigger(trigger_t&& type) noexcept
      : _trigger(std::move(type))
    {
    }

    /// Typed trigger with precision.
    Trigger(trigger_t&& type, const Precision precision) noexcept
      : _trigger(std::move(type))
      , _precision(precision)
    {
    }

    /// Typed trigger with period or frequency.
    Trigger(trigger_t&& type, const PeriodOrFrequency period_or_frequency) noexcept
      : _trigger(std::move(type))
      , _period_or_frequency(period_or_frequency)
    {
    }

    /// Typed trigger with precision and period or frequency.
    Trigger(trigger_t&& type, const Precision precision, const PeriodOrFrequency period_or_frequency) noexcept
      : _trigger(std::move(type))
      , _precision(precision)
      , _period_or_frequency(period_or_frequency)
    {
    }

    Trigger(const Trigger&) = default;
    Trigger(Trigger&&) noexcept = default;
    ~Trigger() = default;
    Trigger& operator=(const Trigger&) = default;
    Trigger& operator=(Trigger&&) noexcept = default;

    /**
     * @return The trigger variant (string name or a typed trigger).
     */
    [[nodiscard]] const trigger_t& trigger() const noexcept { return _trigger; }

    /**
     * @return The precision level associated with the sampling trigger, if set.
     */
    [[nodiscard]] std::optional<Precision> precision() const noexcept { return _precision; }

    /**
     * @return The configured period or frequency for sampling, if set.
     */
    [[nodiscard]] std::optional<PeriodOrFrequency> period_or_frequency() const noexcept { return _period_or_frequency; }

    /**
     * Resolves this trigger to all applicable (pmu_name, event_name, config) tuples.
     * For string triggers, enumerates all PMU variants registered in the counter definition.
     * For typed triggers, delegates to the typed trigger's resolve(), which handles CPU-vendor
     * detection and per-PMU config patching (e.g. ldlat for MemoryLoad).
     *
     * @param counter_definition Counter definition used for event lookup.
     * @return Per-PMU tuples of (pmu_name, event_name, config).
     */
    [[nodiscard]] std::vector<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
      const CounterDefinition& counter_definition) const;

    /**
     * Resolves this trigger for a specific PMU.
     * For string triggers, looks up the event on the given PMU.
     * For typed triggers, delegates to the typed trigger's PMU-specific resolve().
     *
     * @param counter_definition Counter definition used for event lookup.
     * @param pmu_name Name of the PMU to resolve for.
     * @return Tuple of (pmu_name, event_name, config) if the event exists on that PMU.
     */
    [[nodiscard]] std::optional<std::tuple<std::string_view, std::string_view, CounterConfig>> resolve(
      const CounterDefinition& counter_definition,
      std::string_view pmu_name) const;

    /**
     * @return Transforms the trigger into a readable string.
     */
    [[nodiscard]] std::string to_string() const;

  private:
    trigger_t _trigger;
    std::optional<Precision> _precision{ std::nullopt };
    std::optional<PeriodOrFrequency> _period_or_frequency{ std::nullopt };
  };

  /**
   * Represents a counter that is configured to sample;
   * including the counter group (plus counter names) and the buffer
   * user-level buffer that is used by the perf subsystem to store the samples.
   */
  class SampleCounter
  {
  public:
    SampleCounter(Group&& group,
                  const bool has_intel_auxiliary_counter,
                  const bool has_amd_fetch_pmu_counter,
                  const bool has_amd_op_pmu_counter)
      : _group(std::move(group))
      , _has_intel_auxiliary_event(has_intel_auxiliary_counter)
      , _has_amd_ibs_fetch_pmu(has_amd_fetch_pmu_counter)
      , _has_amd_ibs_op_pmu(has_amd_op_pmu_counter)
    {
    }
    SampleCounter(Group&& group,
                  RequestedEventSet&& requested_events,
                  const bool has_auxiliary_counter,
                  const bool has_amd_fetch_pmu_counter,
                  const bool has_amd_op_pmu_counter)
      : _group(std::move(group))
      , _requested_events(std::move(requested_events))
      , _has_intel_auxiliary_event(has_auxiliary_counter)
      , _has_amd_ibs_fetch_pmu(has_amd_fetch_pmu_counter)
      , _has_amd_ibs_op_pmu(has_amd_op_pmu_counter)
    {
    }
    SampleCounter(const SampleCounter&) = delete;
    SampleCounter(SampleCounter&& other) noexcept = default;

    ~SampleCounter();
    SampleCounter& operator=(const SampleCounter&) = delete;
    SampleCounter& operator=(SampleCounter&&) noexcept = default;

    [[nodiscard]] Group& group() noexcept { return _group; }
    [[nodiscard]] const Group& group() const noexcept { return _group; }
    [[nodiscard]] RequestedEventSet& requested_events() noexcept { return _requested_events; }
    [[nodiscard]] const RequestedEventSet& requested_events() const noexcept { return _requested_events; }
    [[nodiscard]] bool has_intel_auxiliary_event() const noexcept { return _has_intel_auxiliary_event; }
    [[nodiscard]] bool has_amd_fetch_pmu_counter() const noexcept { return _has_amd_ibs_fetch_pmu; }
    [[nodiscard]] bool has_amd_op_pmu_counter() const noexcept { return _has_amd_ibs_op_pmu; }

    /**
     * @return User-level buffer of the first counter (if not nullptr) or the second counter.
     */
    [[nodiscard]] std::vector<std::vector<std::byte>> consume_samples();

  private:
    /// Group including the leader that is responsible for sampling.
    Group _group;

    /// List of scheduled events if counter values are sampled.
    RequestedEventSet _requested_events;

    /// Indicates if this counter includes an auxiliary counter that is needed for some Intel architectures.
    bool _has_intel_auxiliary_event{ false };

    /// Indicates if the sampler uses the IbsFetch PMU by AMD's Instruction Based Sampling; this information is used for
    /// parsing raw data.
    bool _has_amd_ibs_fetch_pmu{ false };

    /// Indicates if the sampler uses the IbsOp PMU by AMD's Instruction Based Sampling; this information is used for
    /// parsing raw data.
    bool _has_amd_ibs_op_pmu{ false };
  };

  explicit Sampler(const CounterDefinition& counter_definition, SampleConfig config = {})
    : _counter_definition(counter_definition)
    , _config(config)
  {
  }

  explicit Sampler(SampleConfig config = {})
    : Sampler(CounterDefinition::global(), config)
  {
  }

  Sampler(Sampler&&) noexcept = default;
  Sampler(const Sampler&) = delete;

  ~Sampler() = default;
  Sampler& operator=(const Sampler&) = delete;
  Sampler& operator=(Sampler&&) noexcept = delete;

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @return Sampler
   */
  Sampler& trigger(std::string&& trigger_name)
  {
    return trigger(std::vector<std::vector<Trigger>>{ std::vector{ Trigger{ std::move(trigger_name) } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param precision Precision of the event.
   * @return Sampler
   */
  Sampler& trigger(std::string&& trigger_name, const Precision precision)
  {
    return trigger(std::vector<std::vector<Trigger>>{ std::vector{ Trigger{ std::move(trigger_name), precision } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param period Sampling period of the event.
   * @return Sampler
   */
  Sampler& trigger(std::string&& trigger_name, const Period period)
  {
    return trigger(std::vector<std::vector<Trigger>>{ std::vector{ Trigger{ std::move(trigger_name), period } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param frequency Sampling frequency of the event.
   * @return Sampler
   */
  Sampler& trigger(std::string&& trigger_name, const Frequency frequency)
  {
    return trigger(std::vector<std::vector<Trigger>>{ std::vector{ Trigger{ std::move(trigger_name), frequency } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param precision Precision of the event.
   * @param period Sampling period of the event.
   * @return Sampler
   */
  Sampler& trigger(std::string&& trigger_name, const Precision precision, const Period period)
  {
    return trigger(
      std::vector<std::vector<Trigger>>{ std::vector{ Trigger{ std::move(trigger_name), precision, period } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param precision Precision of the event.
   * @param frequency Sampling frequency of the event.
   * @return Sampler
   */
  Sampler& trigger(std::string&& trigger_name, const Precision precision, const Frequency frequency)
  {
    return trigger(
      std::vector<std::vector<Trigger>>{ std::vector{ Trigger{ std::move(trigger_name), precision, frequency } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger (e.g., MemoryLoad, IbsOp).
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @return Sampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  Sampler& trigger(T&& typed_trigger)
  {
    return trigger(std::vector<std::vector<Trigger>>{
      std::vector{ Trigger{ Trigger::trigger_t{ std::forward<T>(typed_trigger) } } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger (e.g., MemoryLoad, IbsOp).
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param precision Precision of the event.
   * @return Sampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  Sampler& trigger(T&& typed_trigger, const Precision precision)
  {
    return trigger(std::vector<std::vector<Trigger>>{
      std::vector{ Trigger{ Trigger::trigger_t{ std::forward<T>(typed_trigger) }, precision } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger with a sampling period.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param period Sampling period of the event.
   * @return Sampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  Sampler& trigger(T&& typed_trigger, const Period period)
  {
    return trigger(std::vector<std::vector<Trigger>>{
      std::vector{ Trigger{ Trigger::trigger_t{ std::forward<T>(typed_trigger) }, period } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger with a sampling frequency.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param frequency Sampling frequency of the event.
   * @return Sampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  Sampler& trigger(T&& typed_trigger, const Frequency frequency)
  {
    return trigger(std::vector<std::vector<Trigger>>{
      std::vector{ Trigger{ Trigger::trigger_t{ std::forward<T>(typed_trigger) }, frequency } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger with precision and a sampling period.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param precision Precision of the event.
   * @param period Sampling period of the event.
   * @return Sampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  Sampler& trigger(T&& typed_trigger, const Precision precision, const Period period)
  {
    return trigger(std::vector<std::vector<Trigger>>{
      std::vector{ Trigger{ Trigger::trigger_t{ std::forward<T>(typed_trigger) }, precision, period } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger with precision and a sampling frequency.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param precision Precision of the event.
   * @param frequency Sampling frequency of the event.
   * @return Sampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  Sampler& trigger(T&& typed_trigger, const Precision precision, const Frequency frequency)
  {
    return trigger(std::vector<std::vector<Trigger>>{
      std::vector{ Trigger{ Trigger::trigger_t{ std::forward<T>(typed_trigger) }, precision, frequency } } });
  }

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   *
   * @param trigger_names Names of the counters that "trigger" sample recording.
   * @return Sampler
   */
  Sampler& trigger(std::vector<std::string>&& trigger_names)
  {
    return trigger(std::vector<std::vector<std::string>>{ std::move(trigger_names) });
  }

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   * Counters of the outer list will be grouped together, to enable auxiliary counter (e.g., needed
   * for Intel's Sapphire Rapids architecture).
   *
   * @param list_of_triggers Group of names of the counters that "trigger" sample recording.
   * @return Sampler
   */
  Sampler& trigger(std::vector<std::vector<std::string>>&& list_of_triggers);

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   *
   * @param trigger Trigger that "triggers" sample recording.
   * @return Sampler
   */
  Sampler& trigger(Trigger&& trigger)
  {
    return this->trigger(std::vector<std::vector<Trigger>>{ std::vector{ std::move(trigger) } });
  }

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   *
   * @param triggers List of name-precision tuples that "trigger" sample recording.
   * @return Sampler
   */
  Sampler& trigger(std::vector<Trigger>&& triggers)
  {
    return trigger(std::vector<std::vector<Trigger>>{ std::move(triggers) });
  }

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   * Counters of the outer list will be grouped together, to enable auxiliary counter (e.g., needed
   * for Intel's Sapphire Rapids architecture).
   *
   * @param triggers Group of names and precisions of the counters that "trigger" sample recording.
   * @return Sampler
   */
  Sampler& trigger(std::vector<std::vector<Trigger>>&& triggers);

  /**
   * @return Configurations to enable values that will be sampled.
   */
  [[nodiscard]] SampleRecordingValues& values() noexcept { return _values; }

  /**
   * @return Config of the sampler.
   */
  [[nodiscard]] const SampleConfig& config() const noexcept { return _config; }

  /**
   * @return Config of the sampler.
   */
  [[nodiscard]] SampleConfig& config() noexcept { return _config; }

  /**
   * Opens the sampler.
   */
  void open();

  /**
   * Opens and starts recording performance counters.
   */
  void start();

  /**
   * Stops recording performance counters.
   */
  void stop();

  /**
   * Closes the sampler, including mapped buffer.
   */
  void close() noexcept;

  /**
   * @return List of sampled events after closing the sampler.
   */
  [[nodiscard]] SampleResult result(bool sort_by_time = true);

  /**
   * Writes the sampled result into a perf data file that can be read by the "perf report" subcommand.
   *
   * @param output_file_name Name of the perf data file.
   */
  void to_perf_file(std::string_view output_file_name);

private:
  /**
   * Transforms a list of trigger events into a single SampleCounter that includes a group of hardware events.
   *
   * @param pmu_name Name of the PMU for which to build the counter group.
   * @param trigger_group List of triggers to transform.
   * @return Sample counter, consisting of a group of trigger event(s).
   */
  [[nodiscard]] SampleCounter transform_trigger_to_sample_counter(std::string_view pmu_name,
                                                                  const std::vector<Trigger>& trigger_group) const;

  /**
   * Adds the given metric and all dependent events and metrics to the event set and all events to the given group.
   *
   * @param metric Metric to add.
   * @param pmu_name PMU.
   * @param requested_event_set Requested event set.
   * @param group Group.
   */
  void add(std::pair<std::string_view, Metric&> metric,
           std::string_view pmu_name,
           RequestedEventSet& requested_event_set,
           Group& group) const;

  /**
   * Checks if the mem-loads-aux auxiliary counter is needed by the trigger group, which is true for some Intel
   * architectures (e.g., Sapphire Rapids).
   *
   * @param pmu_name Name of the PMU.
   * @param trigger_group List of triggers.
   * @return Pair of bool, the first indicates if the auxiliary counter is needed, the second indicates that the counter
   * is already included in the trigger group.
   */
  [[nodiscard]] std::pair<bool, bool> is_auxiliary_event_needed_and_already_included(
    std::string_view pmu_name,
    const std::vector<Trigger>& trigger_group) const;

  /**
   * Consumes the sample data from the sample counters. This will only happen once; the sample data is reset when
   * starting the sampler (again).
   *
   * @return Reference of the consumed sample data (either consumed now or by an earlier call).
   */
  std::vector<std::vector<std::vector<std::byte>>>& consume_sample_data();

  const CounterDefinition& _counter_definition;

  /// List of triggers. Each trigger will open an individual group of counters.
  /// "Normally", a 1-dimensional list would be enough, but since Intel Sapphire Rapids,
  /// we need auxiliary counters for mem-loads, mem-stores, etc.
  std::vector<std::vector<Trigger>> _triggers;

  /// Values to record into every sample.
  SampleRecordingValues _values;

  /// Perf config.
  SampleConfig _config;

  /// List of counter groups used to sample – will be filled when "opening" the sampler.
  std::vector<SampleCounter> _sample_counter;

  /// Flag if the sampler is already opened, i.e., the events are configured.
  /// This enables the user to open the sampler specifically – or open the
  /// sampler when starting.
  bool _is_opened{ false };

  /// Sample data per sample counter consumed from mmaped buffers. The data will be reset when starting the sampler and
  /// consumed when needing the data the first time (e.g., when calculating the result).
  std::vector<std::vector<std::vector<std::byte>>> _sample_data;
};

/**
 * The MultiSamplerBase is the foundation for samplers that have multiple sub-samplers, for example, MultiThreadSampler.
 */
class MultiSamplerBase
{
public:
  MultiSamplerBase(const MultiSamplerBase&) = delete;
  MultiSamplerBase(MultiSamplerBase&&) noexcept = default;
  virtual ~MultiSamplerBase() = default;
  MultiSamplerBase& operator=(const MultiSamplerBase&) = delete;
  MultiSamplerBase& operator=(MultiSamplerBase&&) noexcept = default;

  /**
   * @return Configurations to enable values that will be sampled.
   */
  [[nodiscard]] SampleRecordingValues& values() noexcept { return _values; }

  /**
   * @return Config of the sampler.
   */
  [[nodiscard]] const SampleConfig& config() const noexcept { return _config; }

  /**
   * @return Config of the sampler.
   */
  [[nodiscard]] SampleConfig& config() noexcept { return _config; }

  /**
   * Closes the sampler, including mapped buffer.
   */
  void close() noexcept
  {
    for (auto& sampler : samplers()) {
      sampler.close();
    }
  }

  /**
   * @return List of sampled events after stopping the sampler.
   */
  [[nodiscard]] SampleResult result(const bool sort_by_time = true) { return result(samplers(), sort_by_time); }

  /**
   * Writes the sampled result into a perf data file that can be read by the "perf report" subcommand.
   *
   * @param output_file_name Name of the perf data file.
   */
  void to_perf_file(std::string_view output_file_name) { to_perf_file(samplers(), output_file_name); }

protected:
  explicit MultiSamplerBase(SampleConfig config)
    : _config(config)
  {
  }

  /**
   * @return A list of multiple samplers.
   */
  [[nodiscard]] virtual std::vector<Sampler>& samplers() noexcept = 0;

  /**
   * @return A list of multiple samplers.
   */
  [[nodiscard]] virtual const std::vector<Sampler>& samplers() const noexcept = 0;

  /**
   * Creates a single result from multiple samplers.
   *
   * @param samplers List of samplers.
   * @param is_sort_by_time Flag to sort the result by timestamp attribute (if sampled).
   *
   * @return Single list of results from all incoming samplers.
   */
  [[nodiscard]] static SampleResult result(std::vector<Sampler>& samplers, bool is_sort_by_time);

  /**
   * Writes the sampled result into a perf data file that can be read by the "perf report" subcommand.
   *
   * @param samplers List of samplers.
   * @param output_file_name Name of the perf data file.
   */
  static void to_perf_file(std::vector<Sampler>& samplers, std::string_view output_file_name);

  /**
   * Initializes the given trigger(s) for the given list of samplers.
   *
   * @param samplers List of samplers.
   * @param trigger_names List of triggers.
   */
  static void trigger(std::vector<Sampler>& samplers, std::vector<std::vector<std::string>>&& trigger_names);

  /**
   * Initializes the given trigger(s) for the given list of samplers.
   *
   * @param samplers List of samplers.
   * @param triggers List of triggers.
   */
  static void trigger(std::vector<Sampler>& samplers, std::vector<std::vector<Sampler::Trigger>>&& triggers);

  /**
   * Initializes the given sampler with values and config.
   *
   * @param sampler Sampler to open.
   */
  void open(Sampler& sampler) const { open(sampler, _config); }

  /**
   * Initializes the given sampler with values and config.
   *
   * @param sampler Sampler to open.
   * @param config Config for that sampler.
   */
  void open(Sampler& sampler, SampleConfig config) const;

  /**
   * Initializes the given sampler with values and config.
   * After initialization, the sampler will be started.
   *
   * @param sampler Sampler to start.
   */
  void start(Sampler& sampler) const { start(sampler, _config); }

  /**
   * Initializes the given sampler with values and config.
   * After initialization, the sampler will be started.
   *
   * @param sampler Sampler to start.
   * @param config Config for that sampler.
   */
  void start(Sampler& sampler, SampleConfig config) const;

  /// Values to record into every sample.
  SampleRecordingValues _values;

  /// Perf config.
  SampleConfig _config;
};

class MultiThreadSampler final : public MultiSamplerBase
{
public:
  MultiThreadSampler(const CounterDefinition& counter_definition, std::uint16_t num_threads, SampleConfig config = {});

  explicit MultiThreadSampler(const std::uint16_t num_threads, SampleConfig config = {})
    : MultiThreadSampler(CounterDefinition::global(), num_threads, config)
  {
  }

  MultiThreadSampler(const MultiThreadSampler&) = delete;
  MultiThreadSampler(MultiThreadSampler&&) noexcept = default;

  ~MultiThreadSampler() override = default;
  MultiThreadSampler& operator=(const MultiThreadSampler&) = delete;
  MultiThreadSampler& operator=(MultiThreadSampler&&) noexcept = default;

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @return MultiThreadSampler
   */
  MultiThreadSampler& trigger(std::string&& trigger_name)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{
      std::vector<Sampler::Trigger>{ Sampler::Trigger{ std::move(trigger_name) } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param precision Precision of the event.
   * @return MultiThreadSampler
   */
  MultiThreadSampler& trigger(std::string&& trigger_name, const Precision precision)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{
      std::vector<Sampler::Trigger>{ Sampler::Trigger{ std::move(trigger_name), precision } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param period Sampling period of the event.
   * @return MultiThreadSampler
   */
  MultiThreadSampler& trigger(std::string&& trigger_name, const class Period period)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{
      std::vector<Sampler::Trigger>{ Sampler::Trigger{ std::move(trigger_name), period } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param frequency Sampling frequency of the event.
   * @return MultiThreadSampler
   */
  MultiThreadSampler& trigger(std::string&& trigger_name, const Frequency frequency)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{
      std::vector<Sampler::Trigger>{ Sampler::Trigger{ std::move(trigger_name), frequency } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param precision Precision of the event.
   * @param period Sampling period of the event.
   * @return MultiThreadSampler
   */
  MultiThreadSampler& trigger(std::string&& trigger_name, const Precision precision, const class Period period)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{
      std::vector<Sampler::Trigger>{ Sampler::Trigger{ std::move(trigger_name), precision, period } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger (e.g., MemoryLoad, IbsOp) with precision.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param precision Precision of the event.
   * @return MultiThreadSampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  MultiThreadSampler& trigger(T&& typed_trigger, const Precision precision)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{ std::vector<Sampler::Trigger>{
      Sampler::Trigger{ Sampler::Trigger::trigger_t{ std::forward<T>(typed_trigger) }, precision } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger with a sampling period.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param period Sampling period of the event.
   * @return MultiThreadSampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  MultiThreadSampler& trigger(T&& typed_trigger, const class Period period)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{ std::vector<Sampler::Trigger>{
      Sampler::Trigger{ Sampler::Trigger::trigger_t{ std::forward<T>(typed_trigger) }, period } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger with a sampling frequency.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param frequency Sampling frequency of the event.
   * @return MultiThreadSampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  MultiThreadSampler& trigger(T&& typed_trigger, const Frequency frequency)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{ std::vector<Sampler::Trigger>{
      Sampler::Trigger{ Sampler::Trigger::trigger_t{ std::forward<T>(typed_trigger) }, frequency } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger with precision and a sampling period.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param precision Precision of the event.
   * @param period Sampling period of the event.
   * @return MultiThreadSampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  MultiThreadSampler& trigger(T&& typed_trigger, const Precision precision, const class Period period)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{ std::vector<Sampler::Trigger>{
      Sampler::Trigger{ Sampler::Trigger::trigger_t{ std::forward<T>(typed_trigger) }, precision, period } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger with precision and a sampling frequency.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param precision Precision of the event.
   * @param frequency Sampling frequency of the event.
   * @return MultiThreadSampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  MultiThreadSampler& trigger(T&& typed_trigger, const Precision precision, const Frequency frequency)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{ std::vector<Sampler::Trigger>{
      Sampler::Trigger{ Sampler::Trigger::trigger_t{ std::forward<T>(typed_trigger) }, precision, frequency } } });
  }

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   *
   * @param trigger_names Names of the counters that "triggers" sample recording.
   * @return MultiThreadSampler
   */
  MultiThreadSampler& trigger(std::vector<std::string>&& trigger_names)
  {
    return trigger(std::vector<std::vector<std::string>>{ std::move(trigger_names) });
  }

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   *
   * @param triggers List of triggers tuples that "trigger" sample recording.
   * @return MultiThreadSampler
   */
  MultiThreadSampler& trigger(std::vector<Sampler::Trigger>&& triggers)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{ std::move(triggers) });
  }

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   * Counters of the outer list will be grouped together, to enable auxiliary counter (e.g., needed
   * for Intel's Sapphire Rapids architecture).
   *
   * @param trigger_names Group of names of the counters that "triggers" sample recording.
   * @return MultiThreadSampler
   */
  MultiThreadSampler& trigger(std::vector<std::vector<std::string>>&& trigger_names)
  {
    MultiSamplerBase::trigger(_thread_local_samplers, std::move(trigger_names));
    return *this;
  }

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   * Counters of the outer list will be grouped together, to enable auxiliary counter (e.g., needed
   * for Intel's Sapphire Rapids architecture).
   *
   * @param triggers Group of names and precisions of the counters that "trigger" sample recording.
   * @return MultiThreadSampler
   */
  MultiThreadSampler& trigger(std::vector<std::vector<Sampler::Trigger>>&& triggers)
  {
    MultiSamplerBase::trigger(_thread_local_samplers, std::move(triggers));
    return *this;
  }

  /**
   * Opens recording performance counters on a specific thread.
   *
   * @param thread_id Id of the thread to start.
   */
  void open(const std::uint16_t thread_id)
  {
    if (thread_id >= _thread_local_samplers.size()) {
      throw ThreadIdOutOfBoundsError{ thread_id };
    }

    MultiSamplerBase::open(_thread_local_samplers[thread_id]);
  }

  /**
   * Opens and starts recording performance counters on a specific thread.
   *
   * @param thread_id Id of the thread to start.
   */
  void start(const std::uint16_t thread_id)
  {
    if (thread_id >= _thread_local_samplers.size()) {
      throw ThreadIdOutOfBoundsError{ thread_id };
    }

    MultiSamplerBase::start(_thread_local_samplers[thread_id]);
  }

  /**
   * Stops recording performance counters for a specific thread.
   *
   * @param thread_id Id of the thread to stop.
   */
  void stop(const std::uint16_t thread_id)
  {
    if (thread_id >= _thread_local_samplers.size()) {
      throw ThreadIdOutOfBoundsError{ thread_id };
    }

    _thread_local_samplers[thread_id].stop();
  }

  /**
   * Stops recording performance counters for all threads.
   */
  void stop()
  {
    for (auto& sampler : _thread_local_samplers) {
      sampler.stop();
    }
  }

private:
  std::vector<Sampler> _thread_local_samplers;

  /**
   * @return A list of multiple samplers.
   */
  [[nodiscard]] std::vector<Sampler>& samplers() noexcept override { return _thread_local_samplers; }

  /**
   * @return A list of multiple samplers.
   */
  [[nodiscard]] const std::vector<Sampler>& samplers() const noexcept override { return _thread_local_samplers; }
};

class MultiCoreSampler final : public MultiSamplerBase
{
public:
  MultiCoreSampler(const CounterDefinition& counter_definition,
                   std::vector<std::uint16_t>&& core_ids,
                   SampleConfig config = {});

  MultiCoreSampler(const CounterDefinition& counter_definition,
                   const std::vector<std::uint16_t>& core_ids,
                   SampleConfig config = {})
    : MultiCoreSampler(counter_definition, std::vector<std::uint16_t>{ core_ids }, config)
  {
  }

  explicit MultiCoreSampler(std::vector<std::uint16_t>&& core_ids, SampleConfig config = {})
    : MultiCoreSampler(CounterDefinition::global(), std::move(core_ids), config)
  {
  }

  explicit MultiCoreSampler(const std::vector<std::uint16_t>& core_ids, SampleConfig config = {})
    : MultiCoreSampler(CounterDefinition::global(), std::vector<std::uint16_t>{ core_ids }, config)
  {
  }

  MultiCoreSampler(const MultiCoreSampler&) = delete;
  MultiCoreSampler(MultiCoreSampler&&) noexcept = default;

  ~MultiCoreSampler() override = default;
  MultiCoreSampler& operator=(const MultiCoreSampler&) = delete;
  MultiCoreSampler& operator=(MultiCoreSampler&&) noexcept = default;

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @return MultiCoreSampler
   */
  MultiCoreSampler& trigger(std::string&& trigger_name)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{
      std::vector<Sampler::Trigger>{ Sampler::Trigger{ std::move(trigger_name) } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param precision Precision of the event.
   * @return MultiCoreSampler
   */
  MultiCoreSampler& trigger(std::string&& trigger_name, const Precision precision)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{
      std::vector<Sampler::Trigger>{ Sampler::Trigger{ std::move(trigger_name), precision } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param period Sampling period of the event.
   * @return MultiCoreSampler
   */
  MultiCoreSampler& trigger(std::string&& trigger_name, const class Period period)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{
      std::vector<Sampler::Trigger>{ Sampler::Trigger{ std::move(trigger_name), period } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param frequency Sampling frequency of the event.
   * @return MultiCoreSampler
   */
  MultiCoreSampler& trigger(std::string&& trigger_name, const Frequency frequency)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{
      std::vector<Sampler::Trigger>{ Sampler::Trigger{ std::move(trigger_name), frequency } } });
  }

  /**
   * Set the trigger for sampling to a single counter.
   *
   * @param trigger_name Name of the counter that "triggers" sample recording.
   * @param precision Precision of the event.
   * @param period Sampling period of the event.
   * @return MultiCoreSampler
   */
  MultiCoreSampler& trigger(std::string&& trigger_name, const Precision precision, const class Period period)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{
      std::vector<Sampler::Trigger>{ Sampler::Trigger{ std::move(trigger_name), precision, period } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger (e.g., MemoryLoad, IbsOp) with precision.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param precision Precision of the event.
   * @return MultiCoreSampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  MultiCoreSampler& trigger(T&& typed_trigger, const Precision precision)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{ std::vector<Sampler::Trigger>{
      Sampler::Trigger{ Sampler::Trigger::trigger_t{ std::forward<T>(typed_trigger) }, precision } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger with a sampling period.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param period Sampling period of the event.
   * @return MultiCoreSampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  MultiCoreSampler& trigger(T&& typed_trigger, const class Period period)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{ std::vector<Sampler::Trigger>{
      Sampler::Trigger{ Sampler::Trigger::trigger_t{ std::forward<T>(typed_trigger) }, period } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger with a sampling frequency.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param frequency Sampling frequency of the event.
   * @return MultiCoreSampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  MultiCoreSampler& trigger(T&& typed_trigger, const Frequency frequency)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{ std::vector<Sampler::Trigger>{
      Sampler::Trigger{ Sampler::Trigger::trigger_t{ std::forward<T>(typed_trigger) }, frequency } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger with precision and a sampling period.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param precision Precision of the event.
   * @param period Sampling period of the event.
   * @return MultiCoreSampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  MultiCoreSampler& trigger(T&& typed_trigger, const Precision precision, const class Period period)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{ std::vector<Sampler::Trigger>{
      Sampler::Trigger{ Sampler::Trigger::trigger_t{ std::forward<T>(typed_trigger) }, precision, period } } });
  }

  /**
   * Set the trigger for sampling to a typed trigger with precision and a sampling frequency.
   *
   * @param typed_trigger Typed trigger that "triggers" sample recording.
   * @param precision Precision of the event.
   * @param frequency Sampling frequency of the event.
   * @return MultiCoreSampler
   */
  template<typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, std::string>, int> = 0>
  MultiCoreSampler& trigger(T&& typed_trigger, const Precision precision, const Frequency frequency)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{ std::vector<Sampler::Trigger>{
      Sampler::Trigger{ Sampler::Trigger::trigger_t{ std::forward<T>(typed_trigger) }, precision, frequency } } });
  }

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   *
   * @param trigger_names Names of the counters that "triggers" sample recording.
   * @return MultiCoreSampler
   */
  MultiCoreSampler& trigger(std::vector<std::string>&& trigger_names)
  {
    return trigger(std::vector<std::vector<std::string>>{ std::move(trigger_names) });
  }

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   *
   * @param triggers List of triggers tuples that "trigger" sample recording.
   * @return MultiCoreSampler
   */
  MultiCoreSampler& trigger(std::vector<Sampler::Trigger>&& triggers)
  {
    return trigger(std::vector<std::vector<Sampler::Trigger>>{ std::move(triggers) });
  }

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   * Counters of the outer list will be grouped together, to enable auxiliary counter (e.g., needed
   * for Intel's Sapphire Rapids architecture).
   *
   * @param trigger_names Group of names of the counters that "triggers" sample recording.
   * @return MultiCoreSampler
   */
  MultiCoreSampler& trigger(std::vector<std::vector<std::string>>&& trigger_names)
  {
    MultiSamplerBase::trigger(_core_local_samplers, std::move(trigger_names));
    return *this;
  }

  /**
   * Set the trigger for sampling to a list of different counters (e.g., mem loads and mem stores).
   * Counters of the outer list will be grouped together, to enable auxiliary counter (e.g., needed
   * for Intel's Sapphire Rapids architecture).
   *
   * @param triggers Group of names and precisions of the counters that "trigger" sample recording.
   * @return MultiCoreSampler
   */
  MultiCoreSampler& trigger(std::vector<std::vector<Sampler::Trigger>>&& triggers)
  {
    MultiSamplerBase::trigger(_core_local_samplers, std::move(triggers));
    return *this;
  }

  /**
   * Opens recording performance counters for all specified cores.
   *
   */
  void open();

  /**
   * Opens and starts recording performance counters for all specified cores.
   */
  void start();

  /**
   * Stops the sampler.
   */
  void stop()
  {
    for (auto& sampler : this->_core_local_samplers) {
      sampler.stop();
    }
  }

private:
  /**
   * @return A list of multiple samplers.
   */
  [[nodiscard]] std::vector<Sampler>& samplers() noexcept override { return _core_local_samplers; }

  /**
   * @return A list of multiple samplers.
   */
  [[nodiscard]] const std::vector<Sampler>& samplers() const noexcept override { return _core_local_samplers; }

  /// List of samplers.
  std::vector<Sampler> _core_local_samplers;

  /// List of core ids the samplers should record on.
  std::vector<std::uint16_t> _core_ids;
};

/**
 * Comparator to order the samples by timestamp after collecting multiple samples from different threads or cores.
 */
class SampleTimestampComparator
{
public:
  bool operator()(const Sample& left, const Sample& right) const
  {
    const auto left_timestamp = left.metadata().timestamp().value_or(std::numeric_limits<std::uint64_t>::max());
    const auto right_timestamp = right.metadata().timestamp().value_or(std::numeric_limits<std::uint64_t>::max());

    return left_timestamp < right_timestamp;
  }
};

/**
 * Compares the given counter with a counter descriptor, i.e., a 3-tuple of PMU name, event name, and event
 * configuration.
 */
class CounterComparator
{
public:
  explicit CounterComparator(const Counter& counter) noexcept
    : _counter(counter)
  {
  }
  CounterComparator(const CounterComparator&) = default;
  CounterComparator(CounterComparator&&) noexcept = default;
  ~CounterComparator() noexcept = default;
  CounterComparator& operator=(const CounterComparator&) = delete;
  CounterComparator& operator=(CounterComparator&&) noexcept = delete;

  [[nodiscard]] bool operator()(
    const std::tuple<std::string_view, std::string_view, CounterConfig>& event_descriptor) const
  {
    return _counter == std::get<2>(event_descriptor);
  }

private:
  const Counter& _counter;
};
}