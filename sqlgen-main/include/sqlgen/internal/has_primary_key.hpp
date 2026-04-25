#ifndef SQLGEN_INTERNAL_HAS_PRIMARY_KEY_HPP_
#define SQLGEN_INTERNAL_HAS_PRIMARY_KEY_HPP_

#include <rfl.hpp>

#include "is_primary_key.hpp"

namespace sqlgen::internal {

template <class T>
struct has_primary_key;

template <class... FieldTypes>
struct has_primary_key<rfl::NamedTuple<FieldTypes...>> {
  constexpr static bool value =
      (false || ... || is_primary_key_v<typename FieldTypes::Type>);
};

template <class T>
constexpr bool has_primary_key_v =
    has_primary_key<rfl::named_tuple_t<T>>::value;

}  // namespace sqlgen::internal

#endif
