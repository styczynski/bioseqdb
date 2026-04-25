#include <algorithm>
#include <perfcpp/exception.hpp>
#include <perfcpp/hardware_info.hpp>
#include <perfcpp/sample/decoder.hpp>
#include <perfcpp/sample/record_file_writer.hpp>
#include <perfcpp/sampler.hpp>
#include <utility>

std::vector<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::Sampler::Trigger::resolve(const CounterDefinition& counter_definition) const
{
  return std::visit(
    [&counter_definition](
      const auto& trigger) -> std::vector<std::tuple<std::string_view, std::string_view, CounterConfig>> {
      if constexpr (std::is_same_v<std::decay_t<decltype(trigger)>, std::string>) {
        /// String triggers: validate and return all PMU variants registered in counter_definition.
        if (counter_definition.is_metric(trigger)) {
          throw MetricNotSupportedAsSamplingTriggerError{ trigger };
        }
        auto result = counter_definition.counter(trigger);
        if (result.empty()) {
          throw CannotFindEventError{ trigger };
        }
        return result;
      } else {
        /// Typed triggers: delegate to the typed trigger's resolve(), which handles
        /// CPU-vendor detection and per-PMU config patching.
        return trigger.resolve(counter_definition);
      }
    },
    this->_trigger);
}

std::string
perf::Sampler::Trigger::to_string() const
{
  return std::visit(
  [](
    const auto& trigger) -> auto {
    if constexpr (std::is_same_v<std::decay_t<decltype(trigger)>, std::string>) {
      return trigger;
    } else {
      return trigger.to_string();
    }
  },
  this->_trigger);
}

std::optional<std::tuple<std::string_view, std::string_view, perf::CounterConfig>>
perf::Sampler::Trigger::resolve(const CounterDefinition& counter_definition, const std::string_view pmu_name) const
{
  return std::visit(
    [&counter_definition,
     &pmu_name](const auto& trigger) -> std::optional<std::tuple<std::string_view, std::string_view, CounterConfig>> {
      if constexpr (std::is_same_v<std::decay_t<decltype(trigger)>, std::string>) {
        /// String triggers: look up the event on the specified PMU.
        if (counter_definition.is_metric(trigger)) {
          throw MetricNotSupportedAsSamplingTriggerError{ trigger };
        }
        return counter_definition.counter(pmu_name, trigger);
      } else {
        /// Typed triggers: delegate to the typed trigger's PMU-specific resolve().
        return trigger.resolve(counter_definition, pmu_name);
      }
    },
    this->_trigger);
}

perf::Sampler::SampleCounter::~SampleCounter()
{
  /// Close the group.
  this->_group.close();
}

perf::Sampler&
perf::Sampler::trigger(std::vector<std::vector<std::string>>&& list_of_triggers)
{
  auto triggers = std::vector<std::vector<Trigger>>{};
  triggers.reserve(list_of_triggers.size());

  /// Turn the list of event names into a list of Sampler::Trigger objects and continue processing there.
  for (auto& trigger_names : list_of_triggers) {
    auto trigger_with_precision = std::vector<Trigger>{};
    std::transform(trigger_names.begin(),
                   trigger_names.end(),
                   std::back_inserter(trigger_with_precision),
                   [](auto& name) { return Trigger{ std::move(name) }; });
    triggers.push_back(std::move(trigger_with_precision));
  }

  return this->trigger(std::move(triggers));
}

perf::Sampler&
perf::Sampler::trigger(std::vector<std::vector<Trigger>>&& list_of_triggers)
{
  /// Deny modifying triggers after the sampler was already opened.
  if (this->_is_opened) {
    throw CannotChangeTriggerWhenSamplerOpenedError{};
  }

  this->_triggers = std::move(list_of_triggers);
  return *this;
}

void
perf::Sampler::open()
{
  /// Measuring any CPU core and any process is invalid, according to the perf subsystem documentation.
  if (this->_config.cpu_core().is_any() && this->_config.process().is_any()) {
    throw InvalidConfigAnyCpuCoreAndAnyProcess{};
  }

  /// Do not open again, if the sampler was already opened.
  /// The is_open flag will be reset on closing the sampler.
  if (std::exchange(this->_is_opened, true)) {
    return;
  }

  /// Build the groups from triggers + events from values.
  for (const auto& trigger_group : this->_triggers) {
    if (trigger_group.empty()) {
      continue;
    }

    /// The first trigger determines which PMUs to open; all triggers in a group share the same PMU set
    /// (e.g. mem-loads and mem-loads-aux both live on the same CPU PMU).
    /// On heterogeneous Intel CPUs (P-cores + E-cores), this yields one SampleCounter per PMU.
    const auto events_for_different_pmus = trigger_group.front().resolve(this->_counter_definition);
    for (const auto& event : events_for_different_pmus) {
      const auto pmu_name = std::get<0>(event);
      auto sample_counter = this->transform_trigger_to_sample_counter(pmu_name, trigger_group);
      this->_sample_counter.push_back(std::move(sample_counter));
    }
  }

  /// Verify that at least one trigger was configured.
  if (this->_sample_counter.empty()) {
    throw CannotStartEmptySamplerError{};
  }

  /// Open the trigger hardware events.
  for (auto& sample_counter : this->_sample_counter) {
    /// Open the group.
    sample_counter.group().open(
      this->_config, sample_counter.has_intel_auxiliary_event(), this->_config.buffer_pages(), this->_values);
  }
}

void
perf::Sampler::start()
{
  /// Clear the sample data.
  this->_sample_data.clear();

  /// Open the groups, if not already done.
  this->open();

  /// Enable the counters to start sampling.
  for (const auto& sample_counter : this->_sample_counter) {
    sample_counter.group().enable();
  }
}

void
perf::Sampler::stop()
{
  /// Disable the counters.
  for (const auto& sample_counter : this->_sample_counter) {
    sample_counter.group().disable();
  }
}

void
perf::Sampler::close() noexcept
{
  if (std::exchange(this->_is_opened, false)) {
    /// Clear all buffers, groups, and event names
    /// in order to enable opening again.
    this->_sample_counter.clear();

    /// Clear the sample data.
    this->_sample_data.clear();
  }
}

perf::Sampler::SampleCounter
perf::Sampler::transform_trigger_to_sample_counter(const std::string_view pmu_name,
                                                   const std::vector<Trigger>& trigger_group) const
{
  /// Group of hardware events.
  auto group = Group{};

  /// List of event names that should be read later from results.
  auto requested_events = RequestedEventSet{};

  /// Check if the auxiliary event is needed and needs to be added.
  const auto [is_auxiliary_event_needed, is_auxiliary_event_included] =
    this->is_auxiliary_event_needed_and_already_included(pmu_name, trigger_group);

  /// If the auxiliary event is needed but not included, add it.
  if (is_auxiliary_event_needed && !is_auxiliary_event_included) {
    if (auto auxiliary_event = this->_counter_definition.counter(pmu_name, std::string_view{ "mem-loads-aux" });
        auxiliary_event.has_value()) {

      /// Read the event config (like event id, etc.).
      auto auxiliary_event_config = std::get<2>(auxiliary_event.value());

      /// The auxiliary event needs constant skid.
      auxiliary_event_config.precision(Precision::MustHaveConstantSkid);

      /// Set the event's period or frequency equal to the first trigger (or fall back to config if not configured).
      auxiliary_event_config.period_or_frequency(
        trigger_group.front().period_or_frequency().value_or(this->_config.period_or_frequency()));

      /// Add the event to the group.
      group.add(auxiliary_event_config);
    } else {
      throw AuxiliaryEventForSamplingNotFoundError{};
    }
  }

  /// Add the trigger(s) to the group. For the most time, this will be a single trigger.
  for (const auto& trigger : trigger_group) {

    /// Resolve this trigger for the specific PMU determined by the leading trigger.
    const auto resolved = trigger.resolve(this->_counter_definition, pmu_name);
    if (!resolved.has_value()) {
      throw CannotFindEventError{ trigger.to_string() };
    }

    auto [var_pmu, event_name, event_config] = resolved.value();

    /// Read the event config (like event id, etc.) and apply per-trigger sampling attributes.
    event_config.precision(static_cast<std::uint8_t>(trigger.precision().value_or(this->_config.precise_ip())));
    event_config.period_or_frequency(trigger.period_or_frequency().value_or(this->_config.period_or_frequency()));

    /// Add the event to the group.
    group.add(event_config);

    /// Notice the event name of the trigger event.
    if (this->_values.is_set(SampleRecordingValues::Field::PerformanceCounter)) {
      requested_events.add(RequestedEvent{ pmu_name, event_name, /* group_id */ 0U, /* position in group */ static_cast<std::uint8_t>(group.size()) });
    }
  }

  /// Add possible events as value to the sample.
  if (this->_values.is_set(SampleRecordingValues::Field::PerformanceCounter)) {
    for (const auto& event_name : this->_values.counters()) {

      /// Check if the event is a true hardware event – if so, just add it to the list.
      if (auto event_config = this->_counter_definition.counter(pmu_name, event_name); event_config.has_value()) {
        /// Add the event to the requested event set.
        /// If the request returns true, the event was indeed added and needs to be added to the group.
        /// The group id provided to the event set is 0 since there is only one group.
        const auto is_added = requested_events.add(RequestedEvent{
          pmu_name, std::get<1>(event_config.value()), /* group_id */ 0U, static_cast<std::uint8_t>(group.size()) });
        if (is_added) {
          group.add(std::get<2>(event_config.value()));
        }
      }

      /// Otherwise, check if the event is a metric. In that case, add all depending hardware events (if not already
      /// done).
      else if (const auto metric = this->_counter_definition.metric(event_name); metric.has_value()) {
        this->add(metric.value(), pmu_name, requested_events, group);
      }

      /// Otherwise, check if the event is a time event. Time events are not supported for sampling; let the user know.
      else if (this->_counter_definition.is_time_event(event_name)) {
        throw TimeEventNotSupportedForSamplingError{ event_name };
      }

      /// Throw an exception if the event is neither a hardware event nor a metric.
      else {
        throw CannotFindEventOrMetricError{ event_name };
      }
    }

    if (!requested_events.empty()) {
      return SampleCounter{ std::move(group),
                            std::move(requested_events),
                            is_auxiliary_event_needed,
                            pmu_name == "ibs_fetch",
                            pmu_name == "ibs_op" };
    }
  }

  return SampleCounter{ std::move(group), is_auxiliary_event_needed, pmu_name == "ibs_fetch", pmu_name == "ibs_op" };
}

void
perf::Sampler::add(const std::pair<std::string_view, Metric&> metric,
                   const std::string_view pmu_name,
                   perf::RequestedEventSet& requested_event_set,
                   perf::Group& group) const
{
  const auto metric_name = std::get<0>(metric);
  /// For metrics, we need to add every hardware event the metric depends on (and check their existence).
  for (const auto& depending_event_name : std::get<1>(metric).required_counter_names()) {
    if (const auto depending_event_config = this->_counter_definition.counter(pmu_name, depending_event_name);
        depending_event_config.has_value()) {

      /// Add the event to the requested event set.
      /// If the request returns true, the event is indeed added and needs to be added to the group.
      /// The group id provided to the event set is 0 since there is only one group.
      const auto is_added = requested_event_set.add(RequestedEvent{ pmu_name,
                                                                    std::get<1>(depending_event_config.value()),
                                                                    /* group_id */ 0U,
                                                                    static_cast<std::uint8_t>(group.size()) });
      if (is_added) {
        group.add(std::get<2>(depending_event_config.value()));
      }
    } else if (const auto depending_metric = this->_counter_definition.metric(depending_event_name);
               depending_metric.has_value()) {
      this->add(depending_metric.value(), pmu_name, requested_event_set, group);
    } else if (this->_counter_definition.is_time_event(depending_event_name)) {
      throw TimeEventNotSupportedForSamplingError{ depending_event_name };
    } else {
      throw CannotFindEventForMetricError{ depending_event_name, metric_name };
    }
  }

  /// Add the metric to the list of scheduled events.
  requested_event_set.add(RequestedEvent{ metric_name, true, RequestedEvent::Type::Metric });
}

std::pair<bool, bool>
perf::Sampler::is_auxiliary_event_needed_and_already_included(const std::string_view pmu_name,
                                                              const std::vector<Trigger>& trigger_group) const
{
  if (!HardwareInfo::is_intel_aux_counter_required()) {
    return { false, false };
  }

  const auto mem_loads_event = this->_counter_definition.counter(pmu_name, std::string_view{ "mem-loads" });
  if (!mem_loads_event.has_value()) {
    return { false, false };
  }

  const auto mem_loads_aux_event = this->_counter_definition.counter(pmu_name, std::string_view{ "mem-loads-aux" });
  if (!mem_loads_aux_event.has_value()) {
    return { false, false };
  }

  const auto& mem_loads_config = std::get<2>(mem_loads_event.value());
  const auto& mem_loads_aux_config = std::get<2>(mem_loads_aux_event.value());

  /// Check if any trigger in the group resolves to the mem-loads event on this PMU.
  /// CounterConfig::operator== compares type and config[0] only, so MemoryLoad with a
  /// custom ldlat (config[1]) still matches the base mem-loads config correctly.
  for (const auto& trigger : trigger_group) {
    const auto resolved = trigger.resolve(this->_counter_definition, pmu_name);
    if (!resolved.has_value()) {
      continue;
    }

    if (std::get<2>(resolved.value()) == mem_loads_config) {
      /// Found a mem-loads trigger; check if the leading trigger is already the auxiliary event.
      const auto first_resolved = trigger_group.front().resolve(this->_counter_definition, pmu_name);
      if (first_resolved.has_value()) {
        return { true, std::get<2>(first_resolved.value()) == mem_loads_aux_config };
      }
      return { true, false };
    }
  }

  return { false, false };
}

perf::SampleResult
perf::Sampler::result(const bool sort_by_time)
{
  /// Consume the sample data, when not already consumed.
  const auto& sample_data = this->consume_sample_data();

  if (this->_sample_counter.size() != sample_data.size()) {
    return SampleResult{};
  }

  auto result = std::vector<Sample>{};

  auto sample_decoder = SampleDecoder{ this->_counter_definition, this->_values };
  for (auto sample_counter_id = 0U; sample_counter_id < this->_sample_counter.size(); ++sample_counter_id) {
    const auto& sample_counter = this->_sample_counter[sample_counter_id];
    const auto& counter_sample_data = sample_data[sample_counter_id];

    /// Decode all samples from the buffers.
    auto samples = sample_decoder.decode(counter_sample_data,
                                         sample_counter.has_amd_op_pmu_counter(),
                                         sample_counter.has_amd_fetch_pmu_counter(),
                                         sample_counter.requested_events(),
                                         sample_counter.group());

    /// Append samples to the entire result.
    std::move(samples.begin(), samples.end(), std::back_inserter(result));
  }

  /// Sort the samples if requested and we can sort by time.
  if (this->_values.is_set(SampleRecordingValues::Field::Timestamp) && sort_by_time) {
    std::sort(result.begin(), result.end(), SampleTimestampComparator{});
  }

  return SampleResult{ this->_values, std::move(result) };
}

void
perf::Sampler::to_perf_file(const std::string_view output_file_name)
{
  RecordFileWriter::write(this->_values, this->_sample_counter, this->consume_sample_data(), output_file_name);
}

std::vector<std::vector<std::vector<std::byte>>>&
perf::Sampler::consume_sample_data()
{
  /// Check if the sample data is not yet consumed (i.e., we store a vector of data equal to the size of the sample
  /// counters).
  if (this->_sample_data.size() != this->_sample_counter.size()) {
    this->_sample_data.reserve(this->_sample_counter.size());
    for (auto& sample_counter : this->_sample_counter) {
      this->_sample_data.push_back(sample_counter.consume_samples());
    }
  }

  return this->_sample_data;
}

std::vector<std::vector<std::byte>>
perf::Sampler::SampleCounter::consume_samples()
{
  /// Normally, the first member will control the sample buffer; however, on some Intel
  /// architectures, an auxiliary event is needed before the "real" event – the "real" event controlling the
  /// buffer is the second one.
  const auto event_index = 0U + static_cast<std::uint8_t>(this->_has_intel_auxiliary_event);
  if (auto& members = this->group().members();
      members.size() > event_index && members[event_index].mmap_buffer() != nullptr) {
    return members[event_index].mmap_buffer()->consume_data();
  }

  return {};
}

perf::SampleResult
perf::MultiSamplerBase::result(std::vector<Sampler>& samplers, const bool is_sort_by_time)
{
  if (!samplers.empty()) {
    auto result = samplers.front().result(/* sort_by_time = */ false);

    /// Merge the results from all samplers (the result of the first sampler is the start point).
    for (auto i = 1U; i < samplers.size(); ++i) {
      auto sampler_result = samplers[i].result(/* sort_by_time = */ false);
      std::move(sampler_result.begin(), sampler_result.end(), std::back_inserter(result));
    }

    /// Sort, if requested and supported by all samplers.
    if (is_sort_by_time) {
      /// Verify that all samplers recorded the timestamp that is needed to sort by time.
      const auto is_time_provided = std::all_of(samplers.begin(), samplers.end(), [](const auto& sampler) {
        return sampler._values.is_set(SampleRecordingValues::Field::Timestamp);
      });

      /// Finally, sort if requested and the samples contain a timestamp.
      if (is_time_provided) {
        std::sort(result.begin(), result.end(), SampleTimestampComparator{});
      }
    }

    return result;
  }

  return SampleResult{};
}

void
perf::MultiSamplerBase::to_perf_file(std::vector<Sampler>& samplers, std::string_view output_file_name)
{
  if (!samplers.empty()) {
    /// Since we cannot modify any samplers data, we copy every sample into a new set.
    auto accumulated_sample_data = samplers.front().consume_sample_data();

    /// Merge the data from all samplers (the result of the first sampler is the start point).
    for (auto i = 1U; i < samplers.size(); ++i) {
      const auto& sample_data = samplers[i].consume_sample_data();

      /// Verify that both samples contain the same number of counters.
      if (accumulated_sample_data.size() == sample_data.size()) {
        for (auto counter_id = 0U; counter_id < sample_data.size(); ++counter_id) {
          const auto& counter_sample_data = sample_data[counter_id];

          /// Append the data for every counter as different counters will have different sample data.
          accumulated_sample_data[counter_id].insert(
            accumulated_sample_data[counter_id].end(), counter_sample_data.begin(), counter_sample_data.end());
        }
      }
    }

    /// Write the result using the first sampler as a template.
    RecordFileWriter::write(
      samplers.front()._values, samplers.front()._sample_counter, accumulated_sample_data, output_file_name);
  }
}

void
perf::MultiSamplerBase::trigger(std::vector<Sampler>& samplers, std::vector<std::vector<std::string>>&& trigger_names)
{
  if (samplers.empty()) {
    return;
  }

  for (auto sampler_id = 0U; sampler_id < samplers.size() - 1U; ++sampler_id) {
    samplers[sampler_id].trigger(std::vector<std::vector<std::string>>{ trigger_names });
  }

  samplers.back().trigger(std::move(trigger_names));
}

void
perf::MultiSamplerBase::trigger(std::vector<Sampler>& samplers, std::vector<std::vector<Sampler::Trigger>>&& triggers)
{
  if (samplers.empty()) {
    return;
  }

  for (auto sampler_id = 0U; sampler_id < samplers.size() - 1U; ++sampler_id) {
    samplers[sampler_id].trigger(std::vector<std::vector<Sampler::Trigger>>{ triggers });
  }

  samplers.back().trigger(std::move(triggers));
}

void
perf::MultiSamplerBase::open(Sampler& sampler, const perf::SampleConfig config) const
{
  sampler._values = _values;
  sampler._config = config;

  sampler.open();
}

void
perf::MultiSamplerBase::start(perf::Sampler& sampler, const perf::SampleConfig config) const
{
  sampler._values = _values;
  sampler._config = config;

  sampler.start();
}

perf::MultiThreadSampler::MultiThreadSampler(const perf::CounterDefinition& counter_definition,
                                             const std::uint16_t num_threads,
                                             const perf::SampleConfig config)
  : MultiSamplerBase(config)
{
  /// Create thread-local samplers without config (will be set when starting).
  for (auto thread_id = 0U; thread_id < num_threads; ++thread_id) {
    this->_thread_local_samplers.emplace_back(counter_definition);
  }
}

perf::MultiCoreSampler::MultiCoreSampler(const perf::CounterDefinition& counter_definition,
                                         std::vector<std::uint16_t>&& core_ids,
                                         perf::SampleConfig config)
  : MultiSamplerBase(config)
  , _core_ids(std::move(core_ids))
{
  /// Record all processes on the CPUs.
  _config.process(Process::Any);

  /// Create thread-local samplers without config (will be set when starting).
  for (auto core_id = 0U; core_id < this->_core_ids.size(); ++core_id) {
    this->_core_local_samplers.emplace_back(counter_definition);
  }
}

void
perf::MultiCoreSampler::open()
{
  for (auto sampler_id = 0U; sampler_id < this->_core_ids.size(); ++sampler_id) {
    auto config = this->_config;
    config.cpu_core(CpuCore{ this->_core_ids[sampler_id] });
    MultiSamplerBase::open(this->_core_local_samplers[sampler_id], config);
  }
}

void
perf::MultiCoreSampler::start()
{
  for (auto sampler_id = 0U; sampler_id < this->_core_ids.size(); ++sampler_id) {
    auto config = this->_config;
    config.cpu_core(CpuCore{ this->_core_ids[sampler_id] });
    MultiSamplerBase::start(this->_core_local_samplers[sampler_id], config);
  }
}
