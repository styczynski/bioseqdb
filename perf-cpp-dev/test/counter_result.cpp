#include <catch2/catch_test_macros.hpp>
#include <perfcpp/counter/result.hpp>
#include <vector>

TEST_CASE("access", "[CounterResult]")
{
  SECTION("empty result")
  {
    auto result = perf::CounterResult{};

    REQUIRE(result.empty());
    REQUIRE(result.size() == 0U);
    REQUIRE(result.begin() == result.end());
    REQUIRE_FALSE(result.get("instructions").has_value());
    REQUIRE_FALSE(result["instructions"].has_value());
  }

  SECTION("get by name")
  {
    auto result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{ { "instructions", 100.0 },
                                                                                         { "cycles", 200.0 } } };

    REQUIRE(result.get("instructions").has_value());
    REQUIRE(result.get("instructions").value() == 100.0);
    REQUIRE(result.get("cycles").has_value());
    REQUIRE(result.get("cycles").value() == 200.0);
    REQUIRE_FALSE(result.get("nonexistent").has_value());
  }

  SECTION("get with package prefix")
  {
    auto result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{ { "cycles", 42.0 } } };

    /// Single slash triggers fallback: "cpu/cycles" -> "cycles".
    REQUIRE(result.get("cpu/cycles").has_value());
    REQUIRE(result.get("cpu/cycles").value() == 42.0);

    /// Double slash does NOT trigger fallback.
    REQUIRE_FALSE(result.get("a/b/c").has_value());

    /// Exact match still works.
    REQUIRE(result.get("cycles").has_value());
  }

  SECTION("operator[]")
  {
    auto result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{ { "instructions", 100.0 },
                                                                                         { "cycles", 200.0 } } };

    REQUIRE(result["instructions"].has_value());
    REQUIRE(result["instructions"].value() == 100.0);
    REQUIRE_FALSE(result["missing"].has_value());
  }

  SECTION("iteration")
  {
    auto result = perf::CounterResult{ std::vector<std::pair<std::string_view, double>>{
      { "instructions", 100.0 }, { "cycles", 200.0 }, { "cache-misses", 50.0 } } };

    REQUIRE(result.size() == 3U);
    REQUIRE_FALSE(result.empty());

    /// Verify order is preserved.
    auto names = std::vector<std::string_view>{};
    auto values = std::vector<double>{};
    for (const auto& [name, value] : result) {
      names.push_back(name);
      values.push_back(value);
    }

    REQUIRE(names[0] == "instructions");
    REQUIRE(names[1] == "cycles");
    REQUIRE(names[2] == "cache-misses");
    REQUIRE(values[0] == 100.0);
    REQUIRE(values[1] == 200.0);
    REQUIRE(values[2] == 50.0);
  }

  SECTION("emplace_back")
  {
    auto result = perf::CounterResult{};
    REQUIRE(result.empty());

    result.emplace_back("instructions", 1.0);
    result.emplace_back("cycles", 2.0);

    REQUIRE(result.size() == 2U);
    REQUIRE(result.get("instructions").has_value());
    REQUIRE(result.get("instructions").value() == 1.0);
    REQUIRE(result.get("cycles").has_value());
    REQUIRE(result.get("cycles").value() == 2.0);
  }
}
