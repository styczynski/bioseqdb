#ifndef SQLGEN_TRANSPILATION_AGGREGATION_HPP_
#define SQLGEN_TRANSPILATION_AGGREGATION_HPP_

#include <string>

#include "AggregationOp.hpp"
#include "As.hpp"
#include "Operation.hpp"
#include "Operator.hpp"
#include "to_transpilation_type.hpp"

namespace sqlgen::transpilation {

/// To be used when we want to count everything.
struct All {};

template <AggregationOp _agg, class _ValueType>
struct Aggregation {
  static constexpr auto agg = _agg;
  using ValueType = _ValueType;

  template <rfl::internal::StringLiteral _new_name>
  auto as() const noexcept {
    using T = std::remove_cvref_t<decltype(*this)>;
    return transpilation::As<T, _new_name>{.val = *this};
  }

  ValueType val;
  bool distinct = false;

  template <class T>
  friend auto operator/(const Aggregation& _op1, const T& _op2) noexcept {
    using OtherType = typename transpilation::ToTranspilationType<
        std::remove_cvref_t<T>>::Type;

    return Operation<Operator::divides, Aggregation, OtherType>{
        .operand1 = _op1, .operand2 = to_transpilation_type(_op2)};
  }

  template <class T>
  friend auto operator-(const Aggregation& _op1, const T& _op2) noexcept {
    using OtherType = typename transpilation::ToTranspilationType<
        std::remove_cvref_t<T>>::Type;

    return Operation<Operator::minus, Aggregation, OtherType>{
        .operand1 = _op1, .operand2 = to_transpilation_type(_op2)};
  }

  template <class T>
  friend auto operator%(const Aggregation& _op1, const T& _op2) noexcept {
    using OtherType = typename transpilation::ToTranspilationType<
        std::remove_cvref_t<T>>::Type;

    return Operation<Operator::mod, Aggregation, OtherType>{
        .operand1 = _op1, .operand2 = to_transpilation_type(_op2)};
  }

  template <class T>
  friend auto operator*(const Aggregation& _op1, const T& _op2) noexcept {
    using OtherType = typename transpilation::ToTranspilationType<
        std::remove_cvref_t<T>>::Type;

    return Operation<Operator::multiplies, Aggregation, OtherType>{
        .operand1 = _op1, .operand2 = to_transpilation_type(_op2)};
  }

  template <class T>
  friend auto operator+(const Aggregation& _op1, const T& _op2) noexcept {
    using OtherType = typename transpilation::ToTranspilationType<
        std::remove_cvref_t<T>>::Type;

    return Operation<Operator::plus, Aggregation, OtherType>{
        .operand1 = _op1, .operand2 = to_transpilation_type(_op2)};
  }
};

template <AggregationOp _agg, class _ValueType>
struct ToTranspilationType<Aggregation<_agg, _ValueType>> {
  using Type = Aggregation<_agg, _ValueType>;

  Type operator()(const Type& _val) const noexcept { return _val; }
};

}  // namespace sqlgen::transpilation

#endif
