#ifndef SQLGEN_INTERNAL_HAS_UNIQUE_COLUMN_HPP_
#define SQLGEN_INTERNAL_HAS_UNIQUE_COLUMN_HPP_

#include <rfl.hpp>

#include "is_unique_column.hpp"

namespace sqlgen::internal {

template <class T>
struct has_unique_column;

template <class... FieldTypes>
struct has_unique_column<rfl::NamedTuple<FieldTypes...>> {
  constexpr static bool value =
      (false || ... || is_unique_column_v<typename FieldTypes::Type>);
};

template <class T>
constexpr bool has_unique_column_v =
    has_unique_column<rfl::named_tuple_t<T>>::value;

}  // namespace sqlgen::internal

#endif
