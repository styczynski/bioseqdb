#pragma once

#include <perfcpp/metric/expression/parser.hpp>
#include <string>
#include <vector>

namespace perf {
class Metric
{
public:
  Metric() noexcept = default;
  Metric(const Metric&) noexcept = default;
  Metric(Metric&&) noexcept = default;

  virtual ~Metric() = default;

  Metric& operator=(const Metric&) noexcept = default;
  Metric& operator=(Metric&&) noexcept = default;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual std::vector<std::string> required_counter_names() const = 0;
  [[nodiscard]] virtual std::optional<double> calculate(const CounterResult& result) const = 0;
};

/**
 * The FormulaMetric takes a string-based formula containing operations (+,-,*,/), constant numbers, and event
 * identifiers. The result of the formula will be calculated based on event counter results.
 */
class FormulaMetric final : public Metric
{
public:
  FormulaMetric(std::string&& name, std::string&& formula)
    : _name(std::move(name))
    , _expression(metric::expression::Parser{ std::move(formula) }.parse())
  {
    _expression->add_required_hardware_counter(this->_required_counter_names);
  }

  [[nodiscard]] std::string name() const override { return _name; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override { return _required_counter_names; }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    return _expression->evaluate(result);
  }

private:
  std::string _name;
  std::unique_ptr<metric::expression::ExpressionInterface> _expression;
  std::vector<std::string> _required_counter_names;
};

/**
 * Measures the average number of cycles required to execute one instruction (CPI). Lower values indicate more efficient
 * instruction execution.
 */
class Gigahertz final : public Metric
{
public:
  [[nodiscard]] std::string name() const override { return "gigahertz"; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override { return { "cycles", "seconds" }; }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    const auto cycles = result.get("cycles");
    const auto seconds = result.get("seconds");

    if (cycles.has_value() && seconds.has_value()) {
      return cycles.value() / seconds.value() / 1000000000.;
    }

    return std::nullopt;
  }
};

/**
 * Measures the average number of cycles required to execute one instruction (CPI). Lower values indicate more efficient
 * instruction execution.
 */
class CyclesPerInstruction final : public Metric
{
public:
  [[nodiscard]] std::string name() const override { return "cycles-per-instruction"; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override
  {
    return { "cycles", "instructions" };
  }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    const auto cycles = result.get("cycles");
    const auto instructions = result.get("instructions");

    if (cycles.has_value() && instructions.has_value()) {
      return cycles.value() / instructions.value();
    }

    return std::nullopt;
  }
};

/**
 * Calculates the average number of instructions executed per cycle (IPC). Higher values suggest better CPU utilization
 * and performance.
 */
class InstructionsPerCycle final : public Metric
{
public:
  [[nodiscard]] std::string name() const override { return "instructions-per-cycle"; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override
  {
    return { "cycles", "instructions" };
  }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    const auto instructions = result.get("instructions");
    const auto cycles = result.get("cycles");

    if (instructions.has_value() && cycles.has_value()) {
      return instructions.value() / cycles.value();
    }

    return std::nullopt;
  }
};

/**
 * Computes the ratio of cache hits to total cache accesses (hits + misses). A higher ratio indicates better cache
 * utilization and efficiency.
 */
class CacheHitRatio final : public Metric
{
public:
  [[nodiscard]] std::string name() const override { return "cache-hit-ratio"; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override
  {
    return { "cache-misses", "cache-references" };
  }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    const auto misses = result.get("cache-misses");
    const auto references = result.get("cache-references");

    if (misses.has_value() && references.has_value()) {
      return (references.value() - misses.value()) / references.value();
    }

    return std::nullopt;
  }
};

/**
 * Measures the power consumption of the entire CPU package in Watts using the RAPL energy-pkg counter.
 */
class WattsPkg final : public Metric
{
public:
  [[nodiscard]] std::string name() const override { return "watts-pkg"; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override { return { "energy-pkg", "seconds" }; }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    const auto energy = result.get("energy-pkg");
    const auto seconds = result.get("seconds");

    if (energy.has_value() && seconds.has_value() && seconds.value() > 0.) {
      return energy.value() / seconds.value();
    }

    return std::nullopt;
  }
};

/**
 * Measures the power consumption of the CPU cores in Watts using the RAPL energy-cores counter.
 */
class WattsCores final : public Metric
{
public:
  [[nodiscard]] std::string name() const override { return "watts-cores"; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override
  {
    return { "energy-cores", "seconds" };
  }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    const auto energy = result.get("energy-cores");
    const auto seconds = result.get("seconds");

    if (energy.has_value() && seconds.has_value() && seconds.value() > 0.) {
      return energy.value() / seconds.value();
    }

    return std::nullopt;
  }
};

/**
 * Measures the power consumption of RAM in Watts using the RAPL energy-ram counter.
 */
class WattsRam final : public Metric
{
public:
  [[nodiscard]] std::string name() const override { return "watts-ram"; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override { return { "energy-ram", "seconds" }; }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    const auto energy = result.get("energy-ram");
    const auto seconds = result.get("seconds");

    if (energy.has_value() && seconds.has_value() && seconds.value() > 0.) {
      return energy.value() / seconds.value();
    }

    return std::nullopt;
  }
};

/**
 * Determines the proportion of cache accesses that resulted in misses. A lower ratio indicates better cache
 * performance.
 */
class CacheMissRatio final : public Metric
{
public:
  [[nodiscard]] std::string name() const override { return "cache-miss-ratio"; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override
  {
    return { "cache-misses", "cache-references" };
  }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    const auto misses = result.get("cache-misses");
    const auto references = result.get("cache-references");

    if (misses.has_value() && references.has_value()) {
      return misses.value() / references.value();
    }

    return std::nullopt;
  }
};

/**
 * Calculates the ratio of data Translation Lookaside Buffer (dTLB) misses to total dTLB accesses for data. Lower values
 * signify better dTLB efficiency.
 */
class DTLBMissRatio final : public Metric
{
public:
  [[nodiscard]] std::string name() const override { return "dTLB-miss-ratio"; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override
  {
    return { "dTLB-loads", "dTLB-load-misses" };
  }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    const auto misses = result.get("dTLB-load-misses");
    const auto loads = result.get("dTLB-loads");

    if (misses.has_value() && loads.has_value()) {
      return misses.value() / loads.value();
    }

    return std::nullopt;
  }
};

/**
 * Measures the ratio of instruction Translation Lookaside Buffer (iTLB) misses to total iTLB accesses for instructions.
 * A lower ratio indicates improved instruction fetching efficiency.
 */
class ITLBMissRatio final : public Metric
{
public:
  [[nodiscard]] std::string name() const override { return "iTLB-miss-ratio"; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override
  {
    return { "iTLB-loads", "iTLB-load-misses" };
  }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    const auto misses = result.get("iTLB-load-misses");
    const auto loads = result.get("iTLB-loads");

    if (misses.has_value() && loads.has_value()) {
      return misses.value() / loads.value();
    }

    return std::nullopt;
  }
};

/**
 * Computes the ratio of Level 1 (L1d) data cache misses to total L1 data cache accesses. Lower values suggest more
 * effective L1 cache performance.
 */
class L1DataMissRatio final : public Metric
{
public:
  [[nodiscard]] std::string name() const override { return "L1-data-miss-ratio"; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override
  {
    return { "L1-dcache-loads", "L1-dcache-load-misses" };
  }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    const auto misses = result.get("L1-dcache-load-misses");
    const auto loads = result.get("L1-dcache-loads");

    if (misses.has_value() && loads.has_value()) {
      return misses.value() / loads.value();
    }

    return std::nullopt;
  }
};

/**
 * Determines the ratio of incorrectly predicted branches to total branches. Lower values are indicative of more
 * accurate branch prediction.
 */
class BranchMissRatio final : public Metric
{
public:
  [[nodiscard]] std::string name() const override { return "branch-miss-ratio"; }
  [[nodiscard]] std::vector<std::string> required_counter_names() const override
  {
    return { "branches", "branch-misses" };
  }
  [[nodiscard]] std::optional<double> calculate(const CounterResult& result) const override
  {
    const auto misses = result.get("branch-misses");
    const auto branches = result.get("branches");

    if (misses.has_value() && branches.has_value()) {
      return misses.value() / branches.value();
    }

    return std::nullopt;
  }
};
}