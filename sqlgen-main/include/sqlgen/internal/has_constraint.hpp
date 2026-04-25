#ifndef SQLGEN_INTERNAL_HAS_CONSTRAINT_HPP_
#define SQLGEN_INTERNAL_HAS_CONSTRAINT_HPP_

#include <type_traits>

#include "is_constraint.hpp"

namespace sqlgen::internal {

template <class T>
struct has_constraint;

template <class... FieldTypes>
struct has_constraint<rfl::NamedTuple<FieldTypes...>> {
  constexpr static bool value =
      (false || ... || is_constraint_v<typename FieldTypes::Type>);
};

template <class T>
constexpr bool has_constraint_v = has_constraint<rfl::named_tuple_t<T>>::value;

}  // namespace sqlgen::internal

#endif