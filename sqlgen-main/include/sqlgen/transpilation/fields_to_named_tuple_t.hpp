#ifndef SQLGEN_TRANSPILATION_FIELDSTONAMEDTUPLET_HPP_
#define SQLGEN_TRANSPILATION_FIELDSTONAMEDTUPLET_HPP_

#include <rfl.hpp>

#include "../Literal.hpp"
#include "../internal/all_same_v.hpp"
#include "Union.hpp"
#include "make_field.hpp"
#include "table_tuple_t.hpp"

namespace sqlgen::transpilation {

template <class T>
struct GetFieldName;

template <class T>
struct GetFieldName {
  static_assert(
      rfl::always_false_v<T>,
      "You must declare a field name for this field using sqlgen::as.");
};

template <rfl::internal::StringLiteral _name>
struct GetFieldName<Literal<_name>> {
  static constexpr rfl::internal::StringLiteral name = _name;
};

template <class StructType, class... FieldTypes>
struct FieldsToNamedTupleType;

template <class StructType, class... FieldTypes>
struct FieldsToNamedTupleType {
  using Type = rfl::NamedTuple<rfl::Field<
      GetFieldName<typename MakeField<StructType, FieldTypes>::Name>::name,
      typename MakeField<StructType, FieldTypes>::Type>...>;
};

template <class StructType, class... FieldTypes>
struct FieldsToNamedTupleType<StructType, rfl::Tuple<FieldTypes...>> {
  using Type = typename FieldsToNamedTupleType<StructType, FieldTypes...>::Type;
};

template <class... SelectTs>
struct FieldsToNamedTupleType<Union<SelectTs...>> {
  using NamedTupleTypes = rfl::Tuple<typename FieldsToNamedTupleType<
      table_tuple_t<typename SelectTs::TableOrQueryType,
                    typename SelectTs::AliasType, typename SelectTs::JoinsType>,
      typename SelectTs::FieldsType>::Type...>;
  static_assert(
      sqlgen::internal::all_same_v<NamedTupleTypes>,
      "All SELECT statements in a UNION must return the same columns with "
      "the same types.");
  using Type = rfl::tuple_element_t<0, NamedTupleTypes>;
};

template <class... SelectTs, class... FieldTypes>
struct FieldsToNamedTupleType<Union<SelectTs...>, FieldTypes...> {
  using Type = typename FieldsToNamedTupleType<
      typename FieldsToNamedTupleType<Union<SelectTs...>>::Type,
      FieldTypes...>::Type;
};

template <class... SelectTs, class... FieldTypes>
struct FieldsToNamedTupleType<Union<SelectTs...>, rfl::Tuple<FieldTypes...>> {
  using Type =
      typename FieldsToNamedTupleType<Union<SelectTs...>, FieldTypes...>::Type;
};

template <class StructType, class... FieldTypes>
using fields_to_named_tuple_t =
    typename FieldsToNamedTupleType<StructType, FieldTypes...>::Type;

}  // namespace sqlgen::transpilation

#endif
