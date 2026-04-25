#ifndef SQLGEN_TRANSPILATION_DYNAMICAGGREGATIONT_HPP_
#define SQLGEN_TRANSPILATION_DYNAMICAGGREGATIONT_HPP_

#include "../dynamic/Aggregation.hpp"
#include "AggregationOp.hpp"

namespace sqlgen::transpilation {

template <AggregationOp agg>
struct DynamicAggregation;

template <>
struct DynamicAggregation<AggregationOp::avg> {
  using Type = dynamic::Aggregation::Avg;
};

template <>
struct DynamicAggregation<AggregationOp::count> {
  using Type = dynamic::Aggregation::Count;
};

template <>
struct DynamicAggregation<AggregationOp::max> {
  using Type = dynamic::Aggregation::Max;
};

template <>
struct DynamicAggregation<AggregationOp::min> {
  using Type = dynamic::Aggregation::Min;
};

template <>
struct DynamicAggregation<AggregationOp::sum> {
  using Type = dynamic::Aggregation::Sum;
};

template <AggregationOp agg>
using dynamic_aggregation_t = typename DynamicAggregation<agg>::Type;

}  // namespace sqlgen::transpilation

#endif
