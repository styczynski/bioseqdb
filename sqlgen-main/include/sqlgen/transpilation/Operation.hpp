#ifndef SQLGEN_TRANSPILATION_OPERATION_HPP_
#define SQLGEN_TRANSPILATION_OPERATION_HPP_

#include <string>
#include <type_traits>

#include "../Result.hpp"
#include "As.hpp"
#include "Condition.hpp"
#include "Operator.hpp"
#include "conditions.hpp"
#include "is_duration.hpp"
#include "to_duration.hpp"
#include "to_transpilation_type.hpp"

namespace sqlgen::transpilation {

/// Simple abstraction to be used for the cast operation.
template <class T>
struct TypeHolder {};

template <Operator _op, class _Operand1Type, class _Operand2Type = Nothing,
          class _Operand3Type = Nothing>
struct Operation {
  static constexpr Operator op = _op;

  using Operand1Type = _Operand1Type;
  using Operand2Type = _Operand2Type;
  using Operand3Type = _Operand3Type;

  Operand1Type operand1;
  Operand2Type operand2;
  Operand3Type operand3;

  template <rfl::internal::StringLiteral _new_name>
  auto as() const noexcept {
    using T = std::remove_cvref_t<decltype(*this)>;
    return transpilation::As<T, _new_name>{.val = *this};
  }

  /// Returns an IS NULL condition.
  auto is_null() const noexcept {
    return make_condition(conditions::is_null(*this));
  }

  /// Returns a IS NOT NULL condition.
  auto is_not_null() const noexcept {
    return make_condition(conditions::is_not_null(*this));
  }

  /// Returns a LIKE condition.
  auto like(const std::string& _pattern) const noexcept {
    return make_condition(conditions::like(*this, _pattern));
  }

  /// Returns a NOT LIKE condition.
  auto not_like(const std::string& _pattern) const noexcept {
    return make_condition(conditions::not_like(*this, _pattern));
  }

  template <class T>
  friend auto operator==(const Operation& _o, const T& _t) {
    return make_condition(conditions::equal(_o, to_transpilation_type(_t)));
  }

  template <class T>
  friend auto operator!=(const Operation& _o, const T& _t) {
    return make_condition(conditions::not_equal(_o, to_transpilation_type(_t)));
  }

  template <class T>
  friend auto operator<(const Operation& _o, const T& _t) {
    return make_condition(
        conditions::lesser_than(_o, to_transpilation_type(_t)));
  }

  template <class T>
  friend auto operator<=(const Operation& _o, const T& _t) {
    return make_condition(
        conditions::lesser_equal(_o, to_transpilation_type(_t)));
  }

  template <class T>
  friend auto operator>(const Operation& _o, const T& _t) {
    return make_condition(
        conditions::greater_than(_o, to_transpilation_type(_t)));
  }

  template <class T>
  friend auto operator>=(const Operation& _o, const T& _t) {
    return make_condition(
        conditions::greater_equal(_o, to_transpilation_type(_t)));
  }

  template <class T>
  friend auto operator/(const Operation& _op1, const T& _op2) noexcept {
    using OtherType = typename transpilation::ToTranspilationType<
        std::remove_cvref_t<T>>::Type;

    return Operation<Operator::divides, Operation, OtherType>{
        .operand1 = _op1, .operand2 = to_transpilation_type(_op2)};
  }

  template <class T>
  friend auto operator-(const Operation& _op1, const T& _op2) noexcept {
    if constexpr (is_duration_v<T>) {
      using DurationType = std::remove_cvref_t<T>;
      return _op1 + DurationType(_op2.count() * (-1));

    } else {
      using OtherType = typename transpilation::ToTranspilationType<
          std::remove_cvref_t<T>>::Type;

      return Operation<Operator::minus, Operation, OtherType>{
          .operand1 = _op1, .operand2 = to_transpilation_type(_op2)};
    }
  }

  template <class T>
  friend auto operator%(const Operation& _op1, const T& _op2) noexcept {
    using OtherType = typename transpilation::ToTranspilationType<
        std::remove_cvref_t<T>>::Type;

    return Operation<Operator::mod, Operation, OtherType>{
        .operand1 = _op1, .operand2 = to_transpilation_type(_op2)};
  }

  template <class T>
  friend auto operator*(const Operation& _op1, const T& _op2) noexcept {
    using OtherType = typename transpilation::ToTranspilationType<
        std::remove_cvref_t<T>>::Type;

    return Operation<Operator::multiplies, Operation, OtherType>{
        .operand1 = _op1, .operand2 = to_transpilation_type(_op2)};
  }

  template <class T>
  friend auto operator+(const Operation& _op1, const T& _op2) noexcept {
    if constexpr (is_duration_v<T>) {
      if constexpr (Operation::op == Operator::date_plus_duration) {
        using DurationType = std::remove_cvref_t<T>;
        const auto op2 =
            rfl::tuple_cat(_op1.operand2, rfl::Tuple<DurationType>(_op2));
        using Op2Type = std::remove_cvref_t<decltype(op2)>;
        return transpilation::Operation<
            transpilation::Operator::date_plus_duration, Operand1Type, Op2Type>{
            .operand1 = _op1.operand1, .operand2 = op2};

      } else {
        using DurationType = std::remove_cvref_t<T>;
        return Operation<Operator::date_plus_duration, Operation,
                         rfl::Tuple<DurationType>>{
            .operand1 = _op1, .operand2 = rfl::Tuple<DurationType>(_op2)};
      }

    } else {
      using OtherType = typename transpilation::ToTranspilationType<
          std::remove_cvref_t<T>>::Type;

      return Operation<Operator::plus, Operation, OtherType>{
          .operand1 = _op1, .operand2 = to_transpilation_type(_op2)};
    }
  }
};

template <Operator _op, class _Operand1Type, class _Operand2Type>
struct ToTranspilationType<Operation<_op, _Operand1Type, _Operand2Type>> {
  using Type = Operation<_op, _Operand1Type, _Operand2Type>;

  Type operator()(const Type& _val) const noexcept { return _val; }
};

}  // namespace sqlgen::transpilation

#endif
