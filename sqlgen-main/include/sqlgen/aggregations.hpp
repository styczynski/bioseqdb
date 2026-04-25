#ifndef SQLGEN_AGGREGATIONS_HPP_
#define SQLGEN_AGGREGATIONS_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "col.hpp"
#include "transpilation/Aggregation.hpp"
#include "transpilation/AggregationOp.hpp"
#include "transpilation/to_transpilation_type.hpp"

namespace sqlgen {

template <class T>
auto avg(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Aggregation<transpilation::AggregationOp::avg, Type>{
      .val = transpilation::to_transpilation_type(_t)};
}

inline auto count() {
  return transpilation::Aggregation<transpilation::AggregationOp::count,
                                    transpilation::All>{};
}

template <rfl::internal::StringLiteral _name>
auto count(const Col<_name>&) {
  return transpilation::Aggregation<transpilation::AggregationOp::count,
                                    transpilation::Col<_name>>{
      .val = transpilation::Col<_name>{}};
}

template <rfl::internal::StringLiteral _name>
auto count_distinct(const Col<_name>&) {
  return transpilation::Aggregation<transpilation::AggregationOp::count,
                                    transpilation::Col<_name>>{
      .val = transpilation::Col<_name>{}, .distinct = true};
}

template <class T>
auto max(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Aggregation<transpilation::AggregationOp::max, Type>{
      .val = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto min(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Aggregation<transpilation::AggregationOp::min, Type>{
      .val = transpilation::to_transpilation_type(_t)};
}

template <class T>
auto sum(const T& _t) {
  using Type =
      typename transpilation::ToTranspilationType<std::remove_cvref_t<T>>::Type;
  return transpilation::Aggregation<transpilation::AggregationOp::sum, Type>{
      .val = transpilation::to_transpilation_type(_t)};
}

}  // namespace sqlgen

#endif
