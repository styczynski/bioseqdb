#ifndef SQLGEN_DYNAMIC_COLUMNORVALUE_HPP_
#define SQLGEN_DYNAMIC_COLUMNORVALUE_HPP_

#include <rfl.hpp>

#include "Column.hpp"
#include "Value.hpp"

namespace sqlgen::dynamic {

using ColumnOrValue = rfl::TaggedUnion<"type", Column, Value>;

}  // namespace sqlgen::dynamic

#endif
