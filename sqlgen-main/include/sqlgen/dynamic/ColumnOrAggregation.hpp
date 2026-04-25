#ifndef SQLGEN_DYNAMIC_COLUMNORAGGREGATION_HPP_
#define SQLGEN_DYNAMIC_COLUMNORAGGREGATION_HPP_

#include <rfl.hpp>

#include "Aggregation.hpp"
#include "Column.hpp"

namespace sqlgen::dynamic {

using ColumnOrAggregation = rfl::TaggedUnion<"type", Column, Aggregation>;

}  // namespace sqlgen::dynamic

#endif
