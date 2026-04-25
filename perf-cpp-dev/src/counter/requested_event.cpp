#include <perfcpp/counter/requested_event.hpp>

bool
perf::RequestedEventSet::add(const RequestedEvent& event)
{
  /// If the event is not already added (in that case adjust_visibility_if_present() will return false), add it.
  /// If the event is already in the set, adjust_visibility_if_present() will adjust the visibility to true, if
  /// is_shown_in_results is true.
  if (!this->adjust_visibility_if_present(event.pmu_name(), event.event_name(), event.is_shown_in_results())) {
    this->_requested_events.push_back(event);
    return true;
  }

  return false;
}

bool
perf::RequestedEventSet::adjust_visibility_if_present(const std::optional<std::string_view> pmu_name,
                                                      const std::string_view event_name,
                                                      const bool is_shown_in_results)
{
  auto iterator = std::find_if(
    this->_requested_events.begin(), this->_requested_events.end(), [pmu_name, event_name](const auto& event) {
      return event.pmu_name() == pmu_name && event.event_name() == event_name;
    });

  /// If the event is not in the set, notify the caller that the event needs to be added.
  if (iterator == this->_requested_events.end()) {
    return false;
  }

  /// If the event should be included into the results, mark it accordingly.
  if (is_shown_in_results) {
    iterator->shown_in_results(true);
  }

  return true;
}

perf::CounterResult
perf::RequestedEventSet::result(const CounterDefinition& counter_definition,
                                CounterResult&& hardware_events_result,
                                const std::uint64_t normalization) const
{
  /// Combine all hardware events and metrics into a single result, showing only the requested events and metrics, in
  /// the requested order. Accordingly, we need to calculate the metrics first, using the given hardware events.
  /// However, since metrics can be referenced recursively (metric_a uses metric_b), we need to resolve the metrics in a
  /// specific order (metric_b before metric_a in this example). To do so, we calculate a metric dependency graph first
  /// and calculate metrics without dependencies, until all metrics are calculated.
  auto metric_graph = this->build_metric_graph(counter_definition);

  /// Check if the metric graph has a cycle. In that case, we cannot evaluate the metrics.
  if (metric_graph.is_cyclic()) {
    throw CannotEvaluateMetricsBecauseOfCycleError{};
  }

  /// Walk through the metric graph, removing one metric without dependencies ata time.
  while (!metric_graph.empty()) {

    /// Get metric without un-calculated dependency.
    if (const auto metric_name = metric_graph.pop(); metric_name.has_value()) {
      if (auto metric = counter_definition.metric(metric_name.value()); metric.has_value()) {

        /// Calculate the metric.
        if (const auto calculated_metric_value = std::get<1>(metric.value()).calculate(hardware_events_result);
            calculated_metric_value.has_value()) {

          /// Add it to the results.
          hardware_events_result.emplace_back(metric_name.value(), calculated_metric_value.value());
        }
      }
    }
  }

  auto event_results = std::vector<std::pair<std::string_view, double>>{};
  event_results.reserve(this->_requested_events.size());

  /// Transform hardware events (now containing also metric results) into a set that is ordered like dictated by the
  /// requested event.
  for (const auto& requested_event : this->_requested_events) {
    if (requested_event.is_shown_in_results()) {
      if (const auto result = hardware_events_result.get(requested_event.event_name()); result.has_value()) {

        /// Normalize hardware and time events.
        if (requested_event.is_hardware_event() || requested_event.is_time_event()) {
          event_results.emplace_back(requested_event.event_name(), result.value() / static_cast<double>(normalization));
        }

        /// Add metrics without normalization.
        else {
          event_results.emplace_back(requested_event.event_name(), result.value());
        }
      }
    }
  }

  return CounterResult{ std::move(event_results) };
}

perf::util::DirectedGraph<std::string_view>
perf::RequestedEventSet::build_metric_graph(const CounterDefinition& counter_definition) const
{
  auto metric_graph = util::DirectedGraph<std::string_view>{};
  for (const auto& requested_event : this->_requested_events) {
    if (const auto metric = counter_definition.metric(requested_event.event_name()); metric.has_value()) {

      /// Add the metric as a node to the graph.
      metric_graph.insert(requested_event.event_name());

      /// Add an edge for every dependent metric: dependent_metric -> metric
      for (const auto& dependency : std::get<1>(metric.value()).required_counter_names()) {
        if (const auto dependent_metric = counter_definition.metric(dependency); dependent_metric.has_value()) {
          metric_graph.connect(std::get<0>(dependent_metric.value()), requested_event.event_name());
        }
      }
    }
  }

  return metric_graph;
}