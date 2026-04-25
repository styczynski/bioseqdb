#ifndef SQLGEN_INTERNAL_REMOVE_AUTO_INCR_PRIMARY_HPP_
#define SQLGEN_INTERNAL_REMOVE_AUTO_INCR_PRIMARY_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "../PrimaryKey.hpp"
#include "../transpilation/is_primary_key.hpp"

namespace sqlgen::internal {

namespace remove_auto_incr {

template <class FieldType>
struct FieldWrapper {};

template <class NamedTupleType>
struct NamedTupleWrapper;

template <class... Fields>
struct NamedTupleWrapper<rfl::NamedTuple<Fields...>> {
  using Type = rfl::NamedTuple<Fields...>;

  template <class NewField>
  friend constexpr auto operator+(const NamedTupleWrapper&,
                                  const FieldWrapper<NewField>&) {
    if constexpr (transpilation::is_primary_key_v<
                      std::remove_pointer_t<typename NewField::Type>>) {
      if constexpr (std::remove_pointer_t<typename NewField::Type>::auto_incr) {
        return NamedTupleWrapper<rfl::NamedTuple<Fields...>>{};
      } else {
        return NamedTupleWrapper<rfl::NamedTuple<Fields..., NewField>>{};
      }
    } else {
      return NamedTupleWrapper<rfl::NamedTuple<Fields..., NewField>>{};
    }
  }
};

template <class NamedTupleType>
struct RemoveAutoIncrPrimary;

template <class... Fields>
struct RemoveAutoIncrPrimary<rfl::NamedTuple<Fields...>> {
  static constexpr auto wrapper =
      (NamedTupleWrapper<rfl::NamedTuple<>>{} + ... + FieldWrapper<Fields>{});

  using Type = decltype(wrapper)::Type;
};

}  // namespace remove_auto_incr

template <class NamedTupleType>
using remove_auto_incr_primary_t =
    typename remove_auto_incr::RemoveAutoIncrPrimary<
        std::remove_cvref_t<NamedTupleType>>::Type;

}  // namespace sqlgen::internal

#endif
