#ifndef SQLGEN_TRANSPILATION_CONDITION_HPP_
#define SQLGEN_TRANSPILATION_CONDITION_HPP_

#include <type_traits>

#include "conditions.hpp"

namespace sqlgen::transpilation {

template <class _ConditionType>
struct Condition {
  using ConditionType = _ConditionType;
  ConditionType condition;

  auto operator!() {
    return make_condition(conditions::Not<_ConditionType>{.cond = condition});
  }

  template <class C2>
  friend auto operator&&(const Condition& _cond1, const Condition<C2>& _cond2) {
    return make_condition(conditions::And<_ConditionType, C2>{
        .cond1 = _cond1.condition, .cond2 = _cond2.condition});
  }

  template <class C2>
  friend auto operator||(const Condition& _cond1, const Condition<C2>& _cond2) {
    return make_condition(conditions::Or<_ConditionType, C2>{
        .cond1 = _cond1.condition, .cond2 = _cond2.condition});
  }
};

template <class T>
auto make_condition(T&& _t) {
  return Condition<std::remove_cvref_t<T>>{.condition = _t};
}

}  // namespace sqlgen::transpilation

#endif
