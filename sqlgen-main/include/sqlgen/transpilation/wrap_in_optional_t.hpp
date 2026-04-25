#ifndef SQLGEN_TRANSPILATION_WRAPINOPTIONALT_HPP_
#define SQLGEN_TRANSPILATION_WRAPINOPTIONALT_HPP_

#include <optional>
#include <rfl.hpp>
#include <type_traits>

#include "is_nullable.hpp"

namespace sqlgen::transpilation {

template <class T>
struct WrapInOptional;

template <class... FieldTypes>
struct WrapInOptional<rfl::NamedTuple<FieldTypes...>> {
  using Type = rfl::NamedTuple<rfl::Field<
      FieldTypes::name_,
      std::conditional_t<is_nullable_v<typename FieldTypes::Type>,
                         typename FieldTypes::Type,
                         std::optional<typename FieldTypes::Type>>>...>;
};

template <class T>
using wrap_in_optional_t = typename WrapInOptional<rfl::named_tuple_t<T>>::Type;

}  // namespace sqlgen::transpilation

#endif
