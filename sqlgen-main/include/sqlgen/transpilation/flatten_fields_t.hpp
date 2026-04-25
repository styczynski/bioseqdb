#ifndef SQLGEN_TRANSPILATION_FLATTEN_FIELDS_T_HPP_
#define SQLGEN_TRANSPILATION_FLATTEN_FIELDS_T_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "As.hpp"
#include "make_field.hpp"
#include "remove_as_t.hpp"

namespace sqlgen::transpilation {

template <class TupleType>
struct TupleWrapper;

template <class... FieldTypes>
struct TupleWrapper<rfl::Tuple<FieldTypes...>> {
  using Type = rfl::Tuple<FieldTypes...>;

  template <class... OtherFieldTypes>
  friend consteval auto operator+(
      const TupleWrapper&,
      const TupleWrapper<rfl::Tuple<OtherFieldTypes...>>&) {
    return TupleWrapper<rfl::Tuple<FieldTypes..., OtherFieldTypes...>>{};
  }
};

template <class StructType, class FieldsType>
struct FlattenFields;

template <class StructType, class FieldType>
struct ExtractFields;

template <class StructType, class FieldType>
  requires(!MakeField<StructType, remove_as_t<FieldType>>::is_operation)
struct ExtractFields<StructType, FieldType> {
  using Type = rfl::Tuple<remove_as_t<FieldType>>;
};

template <class StructType, class FieldType>
  requires(MakeField<StructType, remove_as_t<FieldType>>::is_operation)
struct ExtractFields<StructType, FieldType> {
  using Type = typename FlattenFields<
      StructType,
      typename MakeField<StructType, remove_as_t<FieldType>>::Operands>::Type;
};

template <class StructType, class... FieldTypes>
struct FlattenFields<StructType, rfl::Tuple<FieldTypes...>> {
  static constexpr auto wrapper =
      (TupleWrapper<rfl::Tuple<>>{} + ... +
       TupleWrapper<typename ExtractFields<StructType, FieldTypes>::Type>{});

  using Type = typename decltype(wrapper)::Type;
};

template <class StructType, class FieldsType>
using flatten_fields_t =
    typename FlattenFields<std::remove_cvref_t<StructType>,
                           std::remove_cvref_t<FieldsType>>::Type;

}  // namespace sqlgen::transpilation

#endif
