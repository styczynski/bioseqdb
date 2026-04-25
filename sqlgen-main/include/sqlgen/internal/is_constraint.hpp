#ifndef SQLGEN_INTERNAL_IS_CONSTRAINT_HPP_
#define SQLGEN_INTERNAL_IS_CONSTRAINT_HPP_

#include <type_traits>

#include "is_primary_key.hpp"
#include "is_unique_column.hpp"

namespace sqlgen::internal {

template <class T>
using is_constraint = std::disjunction<is_primary_key<T>, is_unique_column<T>>;

template <class T>
constexpr bool is_constraint_v = is_constraint<std::remove_cvref_t<T>>();

}  // namespace sqlgen::internal

#endif