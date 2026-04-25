#include <catch2/catch_test_macros.hpp>
#include <perfcpp/counter/requested_event.hpp>
#include <perfcpp/metric/metric.hpp>

TEST_CASE("calculating", "[Metric][CyclesPerInstruction]")
{

  auto cpi_metric = perf::CyclesPerInstruction{};

  SECTION("empty result")
  {
    auto counter_result = perf::CounterResult{};
    REQUIRE(cpi_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("wrong/missing hardware counters")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("cycles", 100U), std::make_pair("some-other-event", 500U) } };
    REQUIRE(cpi_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("calculation")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("cycles", 100U), std::make_pair("instructions", 5U), std::make_pair("some-other-event", 500U) } };
    REQUIRE(cpi_metric.calculate(counter_result).has_value());
    REQUIRE(cpi_metric.calculate(counter_result).value() == 20.);
  }
}

TEST_CASE("calculating", "[Metric][InstructionsPerCycle]")
{

  auto ipc_metric = perf::InstructionsPerCycle{};

  SECTION("empty result")
  {
    auto counter_result = perf::CounterResult{};
    REQUIRE(ipc_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("wrong/missing hardware counters")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("cycles", 100U), std::make_pair("some-other-event", 500U) } };
    REQUIRE(ipc_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("calculation")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("cycles", 100U), std::make_pair("instructions", 5U), std::make_pair("some-other-event", 500U) } };
    REQUIRE(ipc_metric.calculate(counter_result).has_value());
    REQUIRE(ipc_metric.calculate(counter_result).value() == .05);
  }
}

TEST_CASE("calculating", "[Metric][CacheHitRatio]")
{
  auto chr_metric = perf::CacheHitRatio{};

  SECTION("empty result")
  {
    auto counter_result = perf::CounterResult{};
    REQUIRE(chr_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("wrong/missing hardware counters")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("cache-misses", 100U), std::make_pair("some-other-event", 500U) } };
    REQUIRE(chr_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("calculation")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("cache-misses", 10U),
      std::make_pair("cache-references", 20U),
      std::make_pair("some-other-event", 500U) } };
    REQUIRE(chr_metric.calculate(counter_result).has_value());
    REQUIRE(chr_metric.calculate(counter_result).value() == .5);
  }
}

TEST_CASE("calculating", "[Metric][CacheMissRatio]")
{
  auto chr_metric = perf::CacheMissRatio{};

  SECTION("empty result")
  {
    auto counter_result = perf::CounterResult{};
    REQUIRE(chr_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("wrong/missing hardware counters")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("cache-misses", 100U), std::make_pair("some-other-event", 500U) } };
    REQUIRE(chr_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("calculation")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("cache-misses", 10U),
      std::make_pair("cache-references", 20U),
      std::make_pair("some-other-event", 500U) } };
    REQUIRE(chr_metric.calculate(counter_result).has_value());
    REQUIRE(chr_metric.calculate(counter_result).value() == .5);
  }
}

TEST_CASE("calculating", "[Metric][DTLBMissRatio]")
{
  auto dtlbmr_metric = perf::DTLBMissRatio{};

  SECTION("empty result")
  {
    auto counter_result = perf::CounterResult{};
    REQUIRE(dtlbmr_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("wrong/missing hardware counters")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("dTLB-load-misses", 100U), std::make_pair("some-other-event", 500U) } };
    REQUIRE(dtlbmr_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("calculation")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("dTLB-load-misses", 10U),
      std::make_pair("dTLB-loads", 20U),
      std::make_pair("some-other-event", 500U) } };
    REQUIRE(dtlbmr_metric.calculate(counter_result).has_value());
    REQUIRE(dtlbmr_metric.calculate(counter_result).value() == .5);
  }
}

TEST_CASE("calculating", "[Metric][ITLBMissRatio]")
{
  auto itlbmr_metric = perf::ITLBMissRatio{};

  SECTION("empty result")
  {
    auto counter_result = perf::CounterResult{};
    REQUIRE(itlbmr_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("wrong/missing hardware counters")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("iTLB-load-misses", 100U), std::make_pair("some-other-event", 500U) } };
    REQUIRE(itlbmr_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("calculation")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("iTLB-load-misses", 10U),
      std::make_pair("iTLB-loads", 20U),
      std::make_pair("some-other-event", 500U) } };
    REQUIRE(itlbmr_metric.calculate(counter_result).has_value());
    REQUIRE(itlbmr_metric.calculate(counter_result).value() == .5);
  }
}

TEST_CASE("calculating", "[Metric][L1DataMissRatio]")
{
  auto l1dmr_metric = perf::L1DataMissRatio{};

  SECTION("empty result")
  {
    auto counter_result = perf::CounterResult{};
    REQUIRE(l1dmr_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("wrong/missing hardware counters")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("L1-dcache-load-misses", 100U), std::make_pair("some-other-event", 500U) } };
    REQUIRE(l1dmr_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("calculation")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("L1-dcache-load-misses", 10U),
      std::make_pair("L1-dcache-loads", 20U),
      std::make_pair("some-other-event", 500U) } };
    REQUIRE(l1dmr_metric.calculate(counter_result).has_value());
    REQUIRE(l1dmr_metric.calculate(counter_result).value() == .5);
  }
}

TEST_CASE("calculating", "[Metric][BranchMissRatio]")
{
  auto bmr_metric = perf::BranchMissRatio{};

  SECTION("empty result")
  {
    auto counter_result = perf::CounterResult{};
    REQUIRE(bmr_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("wrong/missing hardware counters")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("branch-misses", 100U), std::make_pair("some-other-event", 500U) } };
    REQUIRE(bmr_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("calculation")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("branch-misses", 10U),
      std::make_pair("branches", 20U),
      std::make_pair("some-other-event", 500U) } };
    REQUIRE(bmr_metric.calculate(counter_result).has_value());
    REQUIRE(bmr_metric.calculate(counter_result).value() == .5);
  }
}

TEST_CASE("calculating", "[Metric][Formula]")
{
  SECTION("parsing")
  {
    REQUIRE_THROWS(perf::FormulaMetric{ "test", "('event-a'+'event-b'" });
    REQUIRE_THROWS(perf::FormulaMetric{ "test", "(('event-a'+'event-b')" });
    REQUIRE_THROWS(perf::FormulaMetric{ "test", "'event-a'+'event-b" });
    REQUIRE_THROWS(perf::FormulaMetric{ "test", "('event-a'+'event-b)" });
    REQUIRE_THROWS(perf::FormulaMetric{ "test", "()'event-a'+'event-b)" });

    REQUIRE_NOTHROW(perf::FormulaMetric{ "test", "('event-a'+'event-b')" });
  }

  auto formula_metric = perf::FormulaMetric{ "any-formula", "('event-a'*'event-b')/2+13.37+'event-c'" };

  SECTION("empty result")
  {
    auto counter_result = perf::CounterResult{};
    REQUIRE(formula_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("wrong/missing hardware counters")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("event-a", 100U), std::make_pair("some-other-event", 500U) } };
    REQUIRE(formula_metric.calculate(counter_result).has_value() == false);
  }

  SECTION("calculation")
  {
    auto counter_result =
      perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{ std::make_pair("event-a", 10U),
                                                                             std::make_pair("event-b", 20U),
                                                                             std::make_pair("some-other-event", 500U),
                                                                             std::make_pair("event-c", 0U) } };
    REQUIRE(formula_metric.calculate(counter_result).has_value());
    REQUIRE(formula_metric.calculate(counter_result).value() == 113.37);
  }

  SECTION("scientific number")
  {
    auto scientific_metric = perf::FormulaMetric{ "scientific-formular", "'event-a' * 1e5" };

    auto counter_result =
      perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{ std::make_pair("event-a", 20U) } };
    REQUIRE(scientific_metric.calculate(counter_result).has_value());
    REQUIRE(scientific_metric.calculate(counter_result).value() == 2000000.);
  }

  SECTION("negative scientific number")
  {
    auto scientific_metric = perf::FormulaMetric{ "scientific-formular", "'event-a' * 1e-5" };

    auto counter_result =
      perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{ std::make_pair("event-a", 20U) } };
    REQUIRE(scientific_metric.calculate(counter_result).has_value());
    REQUIRE(scientific_metric.calculate(counter_result).value() == 0.0002);
  }

  SECTION("wrong scientific number")
  {
    REQUIRE_THROWS(perf::FormulaMetric{ "scientific-formular", "'event-a' * 1e2e5" });
  }

  SECTION("d_ratio function")
  {
    REQUIRE_THROWS(perf::FormulaMetric{ "d-ratio-formular", "d_ratio()" });
    REQUIRE_THROWS(perf::FormulaMetric{ "d-ratio-formular", "d_ratio('event-a')" });
    REQUIRE_THROWS(perf::FormulaMetric{ "d-ratio-formular", "d_ratio('event-a', )" });
    REQUIRE_THROWS(perf::FormulaMetric{ "d-ratio-formular", "d_ratio('event-a', 'event-b', 'event-c')" });

    auto d_ratio_metric = perf::FormulaMetric{ "d-ratio-formular", "d_ratio('event-a', 'event-b')" };

    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("event-a", 100U), std::make_pair("event-b", 20U) } };
    REQUIRE(d_ratio_metric.calculate(counter_result).has_value());
    REQUIRE(d_ratio_metric.calculate(counter_result).value() == 5.);
  }

  SECTION("sum function")
  {
    REQUIRE_THROWS(perf::FormulaMetric{ "sum-formular", "sum()" });
    REQUIRE_THROWS(perf::FormulaMetric{ "sum-formular", "sum('event-a')" });
    REQUIRE_THROWS(perf::FormulaMetric{ "sum-formular", "sum('event-a',)" });

    auto sum_metric = perf::FormulaMetric{ "sum-formular", "sum(10, 'event-a', 10, 'event-b', 10)" };

    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("event-a", 100U), std::make_pair("event-b", 20U) } };
    REQUIRE(sum_metric.calculate(counter_result).has_value());
    REQUIRE(sum_metric.calculate(counter_result).value() == (10 + 100 + 10 + 20 + 10));
  }

  SECTION("formula with PMU name")
  {
    auto cpi_metric = perf::FormulaMetric{ "cpi", "'cpu/cycles' / instructions" };

    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("cycles", 100U), std::make_pair("instructions", 20U) } };
    REQUIRE(cpi_metric.calculate(counter_result).has_value());
    REQUIRE(cpi_metric.calculate(counter_result).value() == (100U / 20U));
  }
}

TEST_CASE("calculating", "[Metric][NestedMetrics]")
{
  auto counter_definition = perf::CounterDefinition{};
  counter_definition.add("metric-a", "'event-a' + 'event-b'");
  counter_definition.add("metric-b", "'metric-a' + 400");

  SECTION("in-order")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("event-a", 100U), std::make_pair("event-b", 500U) } };

    /// Add metric-a before metric-b and access the metrics in that order.
    auto requested_event_set = perf::RequestedEventSet{};
    requested_event_set.add(perf::RequestedEvent{ "metric-a", true, perf::RequestedEvent::Type::Metric });
    requested_event_set.add(perf::RequestedEvent{ "metric-b", true, perf::RequestedEvent::Type::Metric });

    const auto final_result = requested_event_set.result(counter_definition, std::move(counter_result), 1U);
    REQUIRE(final_result.get("metric-a").has_value());
    REQUIRE(final_result.get("metric-a") == 600U);
    REQUIRE(final_result.get("metric-b").has_value());
    REQUIRE(final_result.get("metric-b") == 1000U);
  }

  SECTION("out-of-order")
  {
    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("event-a", 100U), std::make_pair("event-b", 500U) } };

    /// Add metric-b before metric-a and access the metrics in the opposite order.
    auto requested_event_set = perf::RequestedEventSet{};
    requested_event_set.add(perf::RequestedEvent{ "metric-b", true, perf::RequestedEvent::Type::Metric });
    requested_event_set.add(perf::RequestedEvent{ "metric-a", true, perf::RequestedEvent::Type::Metric });

    const auto final_result = requested_event_set.result(counter_definition, std::move(counter_result), 1U);
    REQUIRE(final_result.get("metric-a").has_value());
    REQUIRE(final_result.get("metric-a") == 600U);
    REQUIRE(final_result.get("metric-b").has_value());
    REQUIRE(final_result.get("metric-b") == 1000U);
  }

  SECTION("cyclic")
  {
    /// Add two metrics that reference each other.
    counter_definition.add("metric-c", "'metric-d' + 'metric-a'");
    counter_definition.add("metric-d", "'metric-c' + 'metric-b'");

    auto counter_result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      std::make_pair("event-a", 100U), std::make_pair("event-b", 500U) } };

    /// Add metric-b before metric-a and access the metrics in the opposite order.
    auto requested_event_set = perf::RequestedEventSet{};
    requested_event_set.add(perf::RequestedEvent{ "metric-c", true, perf::RequestedEvent::Type::Metric });
    requested_event_set.add(perf::RequestedEvent{ "metric-d", true, perf::RequestedEvent::Type::Metric });

    /// Evaluation must throw an exception since the metrics are cyclic.
    REQUIRE_THROWS(requested_event_set.result(counter_definition, std::move(counter_result), 1U));
  }
}