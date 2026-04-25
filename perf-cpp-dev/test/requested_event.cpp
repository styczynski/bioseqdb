#include <catch2/catch_test_macros.hpp>
#include <perfcpp/counter/requested_event.hpp>
#include <perfcpp/counter/result.hpp>
#include <perfcpp/counter_definition.hpp>

TEST_CASE("empty RequestedEventSet", "[RequestedEventSet]")
{
  auto event_set = perf::RequestedEventSet{};
  auto counter_definition = perf::CounterDefinition{};

  SECTION("newly created event set is empty")
  {
    REQUIRE(event_set.empty());
    REQUIRE(event_set.size() == 0U);
    REQUIRE(event_set.begin() == event_set.end());
  }

  SECTION("event set with reserved capacity is still empty")
  {
    auto event_set_with_capacity = perf::RequestedEventSet{ 10U };

    REQUIRE(event_set_with_capacity.empty());
    REQUIRE(event_set_with_capacity.size() == 0U);
    REQUIRE(event_set_with_capacity.begin() == event_set_with_capacity.end());
  }

  SECTION("result with empty hardware_events_result should be empty")
  {
    auto empty_hardware_result = perf::CounterResult{};

    auto result = event_set.result(counter_definition, std::move(empty_hardware_result), 1U);

    REQUIRE(result.begin() == result.end());
    REQUIRE_FALSE(result.get("instructions").has_value());
    REQUIRE_FALSE(result.get("cycles").has_value());
  }

  SECTION("result with non-empty hardware_events_result but no requested events should be empty")
  {
    auto hardware_result =
      perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{ std::make_pair("instructions", 1000.0) } };

    auto result = event_set.result(counter_definition, std::move(hardware_result), 1U);

    REQUIRE(result.begin() == result.end());
    REQUIRE_FALSE(result.get("instructions").has_value());
    REQUIRE_FALSE(result.get("cycles").has_value());
  }

  SECTION("result with reserved capacity - empty hardware_events_result should be empty")
  {
    auto event_set_with_capacity = perf::RequestedEventSet{ 10U };
    auto empty_hardware_result = perf::CounterResult{};

    auto result = event_set_with_capacity.result(counter_definition, std::move(empty_hardware_result), 1U);

    REQUIRE(result.begin() == result.end());
    REQUIRE_FALSE(result.get("instructions").has_value());
    REQUIRE_FALSE(result.get("cycles").has_value());
  }

  SECTION("result with reserved capacity - non-empty hardware_events_result but no requested events should be empty")
  {
    auto event_set_with_capacity = perf::RequestedEventSet{ 10U };
    auto hardware_result =
      perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{ std::make_pair("instructions", 1000.0) } };

    auto result = event_set_with_capacity.result(counter_definition, std::move(hardware_result), 1U);

    REQUIRE(result.begin() == result.end());
    REQUIRE_FALSE(result.get("instructions").has_value());
    REQUIRE_FALSE(result.get("cycles").has_value());
  }
}

TEST_CASE("RequestedEventSet with requested events", "[RequestedEventSet]")
{
  auto counter_definition = perf::CounterDefinition{};

  SECTION("result contains requested event when present in hardware result")
  {
    auto event_set = perf::RequestedEventSet{};

    /// Add the "instructions" event as a hardware event
    REQUIRE(
      event_set.add(perf::RequestedEvent{ "cpu", "instructions", true, perf::RequestedEvent::Type::HardwareEvent }));

    /// Verify the event set is no longer empty
    REQUIRE_FALSE(event_set.empty());
    REQUIRE(event_set.size() == 1U);

    /// Create hardware result containing instructions
    auto hardware_result =
      perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{ std::make_pair("instructions", 1000.0) } };

    auto result = event_set.result(counter_definition, std::move(hardware_result), 1U);

    /// The result should contain the requested instructions event
    REQUIRE(result.get("instructions").has_value());
    REQUIRE(result.get("instructions").value() == 1000.0);

    /// But should not contain unrequested events
    REQUIRE_FALSE(result.get("cycles").has_value());

    /// Verify iterator access
    REQUIRE(result.begin() != result.end());
    auto it = result.begin();
    REQUIRE(it->first == "instructions");
    REQUIRE(it->second == 1000.0);
    ++it;
    REQUIRE(it == result.end());
  }

  SECTION("result is empty when requested event not in hardware result")
  {
    auto event_set = perf::RequestedEventSet{};

    /// Add the "instructions" event as a hardware event
    REQUIRE(
      event_set.add(perf::RequestedEvent{ "cpu", "instructions", true, perf::RequestedEvent::Type::HardwareEvent }));

    /// Create hardware result NOT containing instructions
    auto hardware_result =
      perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{ std::make_pair("cycles", 2000.0) } };

    auto result = event_set.result(counter_definition, std::move(hardware_result), 1U);

    /// The result should be empty since requested event is not in hardware result
    REQUIRE(result.begin() == result.end());
    REQUIRE_FALSE(result.get("instructions").has_value());
    REQUIRE_FALSE(result.get("cycles").has_value());
  }

  SECTION("result respects normalization for hardware events")
  {
    auto event_set = perf::RequestedEventSet{};

    /// Add the "instructions" event as a hardware event
    REQUIRE(
      event_set.add(perf::RequestedEvent{ "cpu", "instructions", true, perf::RequestedEvent::Type::HardwareEvent }));

    /// Create hardware result containing instructions
    auto hardware_result =
      perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{ std::make_pair("instructions", 1000.0) } };

    auto result = event_set.result(counter_definition, std::move(hardware_result), 10U);

    /// The result should be normalized (1000.0 / 10 = 100.0)
    REQUIRE(result.get("instructions").has_value());
    REQUIRE(result.get("instructions").value() == 100.0);
  }
}