#ifndef SQLGEN_TRANSPILATION_CONDITIONS_HPP_
#define SQLGEN_TRANSPILATION_CONDITIONS_HPP_

#include <rfl.hpp>

#include "string_t.hpp"

namespace sqlgen::transpilation::conditions {

template <class CondType1, class CondType2>
struct And {
  using ResultType = bool;

  CondType1 cond1;
  CondType2 cond2;
};

template <class OpType1, class OpType2>
struct Equal {
  using ResultType = bool;

  OpType1 op1;
  OpType2 op2;
};

template <class OpType1, class OpType2>
auto equal(const OpType1& _op1, const OpType2& _op2) {
  return Equal<std::remove_cvref_t<OpType1>, std::remove_cvref_t<OpType2>>{
      .op1 = _op1, .op2 = _op2};
}

template <class OpType1, class OpType2>
struct GreaterEqual {
  using ResultType = bool;

  OpType1 op1;
  OpType2 op2;
};

template <class OpType1, class OpType2>
auto greater_equal(const OpType1& _op1, const OpType2& _op2) {
  return GreaterEqual<std::remove_cvref_t<OpType1>,
                      std::remove_cvref_t<OpType2>>{.op1 = _op1, .op2 = _op2};
}

template <class OpType1, class OpType2>
struct GreaterThan {
  using ResultType = bool;

  OpType1 op1;
  OpType2 op2;
};

template <class OpType1, class OpType2>
auto greater_than(const OpType1& _op1, const OpType2& _op2) {
  return GreaterThan<std::remove_cvref_t<OpType1>,
                     std::remove_cvref_t<OpType2>>{.op1 = _op1, .op2 = _op2};
}

template <class OpType, class... Ts>
struct In {
  using ResultType = bool;

  OpType op;
  rfl::Tuple<Ts...> patterns;
};

template <class OpType, class... Ts>
auto in(const OpType& _op, const Ts&... _ts) {
  return In<OpType, string_t<Ts>...>{
      .op = _op, .patterns = rfl::Tuple<string_t<Ts>...>(_ts...)};
}

template <class OpType, class T>
struct InVec {
  using ResultType = bool;

  OpType op;
  std::vector<T> patterns;
};

template <class OpType, class T>
auto in(const OpType& _op, const std::vector<T>& _patterns) {
  return InVec<OpType, T>{.op = _op, .patterns = _patterns};
}

template <class OpType>
struct IsNull {
  using ResultType = bool;

  OpType op;
};

template <class OpType>
auto is_null(const OpType& _op) {
  return IsNull<std::remove_cvref_t<OpType>>{.op = _op};
}

template <class OpType>
struct IsNotNull {
  using ResultType = bool;

  OpType op;
};

template <class OpType>
auto is_not_null(const OpType& _op) {
  return IsNotNull<std::remove_cvref_t<OpType>>{.op = _op};
}

template <class OpType1, class OpType2>
struct LesserEqual {
  using ResultType = bool;

  OpType1 op1;
  OpType2 op2;
};

template <class OpType1, class OpType2>
auto lesser_equal(const OpType1& _op1, const OpType2& _op2) {
  return LesserEqual<std::remove_cvref_t<OpType1>,
                     std::remove_cvref_t<OpType2>>{.op1 = _op1, .op2 = _op2};
}

template <class OpType1, class OpType2>
struct LesserThan {
  using ResultType = bool;

  OpType1 op1;
  OpType2 op2;
};

template <class OpType1, class OpType2>
auto lesser_than(const OpType1& _op1, const OpType2& _op2) {
  return LesserThan<std::remove_cvref_t<OpType1>, std::remove_cvref_t<OpType2>>{
      .op1 = _op1, .op2 = _op2};
}

template <class OpType>
struct Like {
  using ResultType = bool;

  OpType op;
  std::string pattern;
};

template <class OpType>
auto like(const OpType& _op, const std::string& _pattern) {
  return Like<std::remove_cvref_t<OpType>>{.op = _op, .pattern = _pattern};
}

template <class OpType1, class OpType2>
struct NotEqual {
  using ResultType = bool;

  OpType1 op1;
  OpType2 op2;
};

template <class CondType>
struct Not {
  using ResultType = bool;

  CondType cond;
};

template <class OpType1, class OpType2>
auto not_equal(const OpType1& _op1, const OpType2& _op2) {
  return NotEqual<std::remove_cvref_t<OpType1>, std::remove_cvref_t<OpType2>>{
      .op1 = _op1, .op2 = _op2};
}

template <class OpType, class... Ts>
struct NotIn {
  using ResultType = bool;

  OpType op;
  rfl::Tuple<Ts...> patterns;
};

template <class OpType, class... Ts>
auto not_in(const OpType& _op, const Ts&... _ts) {
  return NotIn<OpType, string_t<Ts>...>{
      .op = _op, .patterns = rfl::Tuple<string_t<Ts>...>(_ts...)};
}

template <class OpType, class T>
struct NotInVec {
  using ResultType = bool;

  OpType op;
  std::vector<T> patterns;
};

template <class OpType, class T>
auto not_in(const OpType& _op, const std::vector<T>& _patterns) {
  return NotInVec<OpType, T>{.op = _op, .patterns = _patterns};
}

template <class OpType>
struct NotLike {
  using ResultType = bool;

  OpType op;
  std::string pattern;
};

template <class OpType>
auto not_like(const OpType& _op, const std::string& _pattern) {
  return NotLike<std::remove_cvref_t<OpType>>{.op = _op, .pattern = _pattern};
}

template <class CondType1, class CondType2>
struct Or {
  using ResultType = bool;

  CondType1 cond1;
  CondType2 cond2;
};

}  // namespace sqlgen::transpilation::conditions

#endif
