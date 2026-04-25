#include <perfcpp/metric/expression/expression.hpp>

std::optional<double>
perf::metric::expression::AdditionExpression::evaluate(const std::optional<double> left,
                                                       const std::optional<double> right) const
{
  if (left.has_value() && right.has_value()) {
    return left.value() + right.value();
  }

  return std::nullopt;
}

std::optional<double>
perf::metric::expression::SubtractionExpression::evaluate(const std::optional<double> left,
                                                          const std::optional<double> right) const
{
  if (left.has_value() && right.has_value()) {
    return left.value() - right.value();
  }

  return std::nullopt;
}

std::optional<double>
perf::metric::expression::MultiplyExpression::evaluate(const std::optional<double> left,
                                                       const std::optional<double> right) const
{
  if (left.has_value() && right.has_value()) {
    return left.value() * right.value();
  }

  return std::nullopt;
}

std::optional<double>
perf::metric::expression::DivideExpression::evaluate(const std::optional<double> left,
                                                     const std::optional<double> right) const
{
  if (left.has_value() && right.has_value()) {
    return left.value() / right.value();
  }

  return std::nullopt;
}