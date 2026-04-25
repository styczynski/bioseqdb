#ifndef SQLGEN_OPERATIONS_HPP_
#define SQLGEN_OPERATIONS_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "col.hpp"
#include "transpilation/Operation.hpp"
#include "transpilation/Operator.hpp"
#include "transpilation/to_transpilation_type.hpp"

namespace sqlgen {

template <class T>
auto abs(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::abs, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class TargetType, class T>
auto cast(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<
      transpilation::Operator::cast, Type,
      transpilation::TypeHolder<std::remove_cvref_t<TargetType>>>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto ceil(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::ceil, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class... Ts>
auto coalesce(const Ts&... _ts) {
  static_assert(sizeof...(_ts) > 1,
                "coalesce(...) must have at least two inputs.");
  using Type = rfl::Tuple<typename transpilation::ToTranspilationType<
      std::remove_cvref_t<Ts>>::Type...>;
  return transpilation::Operation<transpilation::Operator::coalesce, Type>{
      .operand1 = Type(transpilation::to_transpilation_type(_ts)...)};
}

template <class... Ts>
auto concat(const Ts&... _ts) {
  static_assert(sizeof...(_ts) > 0,
                "concat(...) must have at least one input.");
  using Type = rfl::Tuple<typename transpilation::ToTranspilationType<
      std::remove_cvref_t<Ts>>::Type...>;
  return transpilation::Operation<transpilation::Operator::concat, Type>{
      .operand1 = Type(transpilation::to_transpilation_type(_ts)...)};
}

template <class T>
auto cos(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::cos, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto day(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::day, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T, class U>
auto days_between(const T& _t, const U& _u) {
  using Type1 =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  using Type2 =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<U>>::Type;
  return transpilation::Operation<transpilation::Operator::days_between, Type1,
                                  Type2>{
      .operand1 = transpilation::to_transpilation_type(_t),
      .operand2 = transpilation::to_transpilation_type(_u)};
}

template <class T>
auto exp(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::exp, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto floor(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::floor, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto hour(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::hour, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto length(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::length, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto ln(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::ln, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto log2(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::log2, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto lower(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::lower, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T, class U>
auto ltrim(const T& _t, const U& _u) {
  using Type1 =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  using Type2 =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<U>>::Type;
  return transpilation::Operation<transpilation::Operator::ltrim, Type1, Type2>{
      .operand1 = transpilation::to_transpilation_type(_t),
      .operand2 = transpilation::to_transpilation_type(_u)};
}

template <class T>
auto ltrim(const T& _t) {
  return ltrim(_t, std::string(" "));
}

template <class T>
auto minute(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::minute, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto month(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::month, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class StringType, class FromType, class ToType>
auto replace(const StringType& _str, const FromType& _from, const ToType& _to) {
  using Type1 = typename transpilation::ToTranspilationType<
      std::remove_cvref_t<StringType>>::Type;
  using Type2 = typename transpilation::ToTranspilationType<
      std::remove_cvref_t<FromType>>::Type;
  using Type3 = typename transpilation::ToTranspilationType<
      std::remove_cvref_t<ToType>>::Type;
  return transpilation::Operation<transpilation::Operator::replace, Type1,
                                  Type2, Type3>{
      .operand1 = transpilation::to_transpilation_type(_str),
      .operand2 = transpilation::to_transpilation_type(_from),
      .operand3 = transpilation::to_transpilation_type(_to)};
}

template <class T, class U>
auto round(const T& _t, const U& _u) {
  using Type1 =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  using Type2 =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<U>>::Type;
  return transpilation::Operation<transpilation::Operator::round, Type1, Type2>{
      .operand1 = transpilation::to_transpilation_type(_t),
      .operand2 = transpilation::to_transpilation_type(_u)};
}

template <class T>
auto round(const T& _t) {
  return round(_t, 0);
}

template <class T, class U>
auto rtrim(const T& _t, const U& _u) {
  using Type1 =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  using Type2 =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<U>>::Type;
  return transpilation::Operation<transpilation::Operator::rtrim, Type1, Type2>{
      .operand1 = transpilation::to_transpilation_type(_t),
      .operand2 = transpilation::to_transpilation_type(_u)};
}

template <class T>
auto rtrim(const T& _t) {
  return rtrim(_t, std::string(" "));
}

template <class T>
auto second(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::second, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto sin(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::sin, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto sqrt(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::sqrt, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto tan(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::tan, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T, class U>
auto trim(const T& _t, const U& _u) {
  using Type1 =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  using Type2 =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<U>>::Type;
  return transpilation::Operation<transpilation::Operator::trim, Type1, Type2>{
      .operand1 = transpilation::to_transpilation_type(_t),
      .operand2 = transpilation::to_transpilation_type(_u)};
}

template <class T>
auto trim(const T& _t) {
  return trim(_t, std::string(" "));
}

template <class T>
auto unixepoch(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::unixepoch, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto upper(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::upper, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto weekday(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::weekday, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto year(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Operation<transpilation::Operator::year, Type>{
      .operand1 = transpilation::to_transpilation_type(_t)};
}

}  // namespace sqlgen

#endif
