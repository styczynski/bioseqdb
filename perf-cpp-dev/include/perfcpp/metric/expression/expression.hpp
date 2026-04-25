#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <perfcpp/counter/result.hpp>
#include <perfcpp/metric/expression/token.hpp>
#include <string>
#include <vector>

namespace perf::metric::expression {
/**
 * The interface for all evaluable metric expressions.
 */
class ExpressionInterface
{
public:
  ExpressionInterface() noexcept = default;
  ExpressionInterface(const ExpressionInterface&) = default;
  ExpressionInterface(ExpressionInterface&&) noexcept = default;
  ExpressionInterface& operator=(const ExpressionInterface&) = default;
  ExpressionInterface& operator=(ExpressionInterface&&) noexcept = default;
  virtual ~ExpressionInterface() = default;

  /**
   * Evaluates the metric with respect to the provided (hardware) counter result.
   *
   * @param result Result of hardware counters.
   * @return The evaluated metric, if all required counters are available.
   */
  [[nodiscard]] virtual std::optional<double> evaluate(const CounterResult& result) const = 0;

  /**
   * Adds names of hardware counters that are needed for this metric.
   * @param hardware_counter_names Names of hardware counters needed to calculate the metric.
   */
  virtual void add_required_hardware_counter(std::vector<std::string>& hardware_counter_names) const = 0;
};

/**
 * Representation of a constant in a metric expression.
 */
class ConstantExpression final : public ExpressionInterface
{
public:
  explicit ConstantExpression(const double value) noexcept
    : _value(value)
  {
  }

  ConstantExpression(const ConstantExpression&) = default;
  ConstantExpression(ConstantExpression&&) noexcept = default;
  ConstantExpression& operator=(const ConstantExpression&) = delete;
  ConstantExpression& operator=(ConstantExpression&&) noexcept = delete;
  ~ConstantExpression() noexcept override = default;

  /**
   * @return The constant.
   */
  [[nodiscard]] std::optional<double> evaluate(const CounterResult& /* result */) const override { return _value; }

  /**
   * For constant expressions, no further hardware counters are required.
   */
  void add_required_hardware_counter(std::vector<std::string>& /* hardware_counter_names */) const override {}

private:
  const double _value;
};

/**
 * Resolves an identifier and returns the hardware/time event value of that identifier.
 */
class IdentifierExpression final : public ExpressionInterface
{
public:
  explicit IdentifierExpression(std::string&& identifier)
    : _identifier(std::move(identifier))
  {
  }

  IdentifierExpression(const IdentifierExpression&) = default;
  IdentifierExpression(IdentifierExpression&&) noexcept = default;
  IdentifierExpression& operator=(const IdentifierExpression&) = delete;
  IdentifierExpression& operator=(IdentifierExpression&&) noexcept = delete;
  ~IdentifierExpression() override = default;

  /**
   * An identifier refers to a specific counter or metric, i.e., evaluate returns the value of that counter or metric.
   *
   * @param result Result of hardware counters.
   * @return The value of the counter/metric specified by the identifier.
   */
  [[nodiscard]] std::optional<double> evaluate(const CounterResult& result) const override
  {
    return result.get(_identifier);
  }

  /**
   * Adds the identifier as required counter.
   * @param hardware_counter_names List of required counters that will be augmented with the identifier.
   */
  void add_required_hardware_counter(std::vector<std::string>& hardware_counter_names) const override
  {
    hardware_counter_names.push_back(_identifier);
  }

private:
  const std::string _identifier;
};

/**
 * Abstract implementation of binary expressions to simplify the implementation of specific operations like addition.
 */
class BinaryExpression : public ExpressionInterface
{
public:
  BinaryExpression(std::unique_ptr<ExpressionInterface>&& left, std::unique_ptr<ExpressionInterface>&& right)
    : _left(std::move(left))
    , _right(std::move(right))
  {
  }

  BinaryExpression(const BinaryExpression&) = delete;
  BinaryExpression(BinaryExpression&&) noexcept = default;
  BinaryExpression& operator=(const BinaryExpression&) = delete;
  BinaryExpression& operator=(BinaryExpression&&) noexcept = default;
  ~BinaryExpression() override = default;

  /**
   * Evaluates a binary expression recursively, i.e., all sub expressions are also evaluated.
   *
   * @param result List of results.
   * @return The evaluated metric, if all required counters are available in the result.
   */
  [[nodiscard]] std::optional<double> evaluate(const CounterResult& result) const override
  {
    return evaluate(this->_left->evaluate(result), this->_right->evaluate(result));
  }

  /**
   * Adds all counters for both branches of the binary expression.
   *
   * @param hardware_counter_names List of hardware counters that will be augmented.
   */
  void add_required_hardware_counter(std::vector<std::string>& hardware_counter_names) const override
  {
    _left->add_required_hardware_counter(hardware_counter_names);
    _right->add_required_hardware_counter(hardware_counter_names);
  }

protected:
  [[nodiscard]] virtual std::optional<double> evaluate(std::optional<double> left,
                                                       std::optional<double> right) const = 0;

private:
  std::unique_ptr<ExpressionInterface> _left;
  std::unique_ptr<ExpressionInterface> _right;
};

/**
 * Performs an addition of both operands.
 */
class AdditionExpression final : public BinaryExpression
{
public:
  using BinaryExpression::evaluate;

  AdditionExpression(std::unique_ptr<ExpressionInterface>&& left, std::unique_ptr<ExpressionInterface>&& right)
    : BinaryExpression(std::move(left), std::move(right))
  {
  }

  AdditionExpression(const AdditionExpression&) = delete;
  AdditionExpression(AdditionExpression&&) noexcept = default;
  AdditionExpression& operator=(const AdditionExpression&) = delete;
  AdditionExpression& operator=(AdditionExpression&&) noexcept = default;
  ~AdditionExpression() override = default;

protected:
  [[nodiscard]] std::optional<double> evaluate(std::optional<double> left, std::optional<double> right) const override;
};

/**
 * Performs a subtraction of both operands.
 */
class SubtractionExpression final : public BinaryExpression
{
public:
  using BinaryExpression::evaluate;

  SubtractionExpression(std::unique_ptr<ExpressionInterface>&& left, std::unique_ptr<ExpressionInterface>&& right)
    : BinaryExpression(std::move(left), std::move(right))
  {
  }

  SubtractionExpression(const SubtractionExpression&) = delete;
  SubtractionExpression(SubtractionExpression&&) noexcept = default;
  SubtractionExpression& operator=(const SubtractionExpression&) = delete;
  SubtractionExpression& operator=(SubtractionExpression&&) noexcept = default;
  ~SubtractionExpression() override = default;

protected:
  [[nodiscard]] std::optional<double> evaluate(std::optional<double> left, std::optional<double> right) const override;
};

/**
 * Multiplies both operands.
 */
class MultiplyExpression final : public BinaryExpression
{
public:
  using BinaryExpression::evaluate;

  MultiplyExpression(std::unique_ptr<ExpressionInterface>&& left, std::unique_ptr<ExpressionInterface>&& right)
    : BinaryExpression(std::move(left), std::move(right))
  {
  }

  MultiplyExpression(const MultiplyExpression&) = delete;
  MultiplyExpression(MultiplyExpression&&) noexcept = default;
  MultiplyExpression& operator=(const MultiplyExpression&) = delete;
  MultiplyExpression& operator=(MultiplyExpression&&) noexcept = default;
  ~MultiplyExpression() override = default;

protected:
  [[nodiscard]] std::optional<double> evaluate(std::optional<double> left, std::optional<double> right) const override;
};

/**
 * Divides both operands.
 */
class DivideExpression final : public BinaryExpression
{
public:
  using BinaryExpression::evaluate;

  DivideExpression(std::unique_ptr<ExpressionInterface>&& left, std::unique_ptr<ExpressionInterface>&& right)
    : BinaryExpression(std::move(left), std::move(right))
  {
  }

  DivideExpression(const DivideExpression&) = delete;
  DivideExpression(DivideExpression&&) noexcept = default;
  DivideExpression& operator=(const DivideExpression&) = delete;
  DivideExpression& operator=(DivideExpression&&) noexcept = default;
  ~DivideExpression() override = default;

protected:
  [[nodiscard]] std::optional<double> evaluate(std::optional<double> left, std::optional<double> right) const override;
};
}