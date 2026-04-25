#ifndef SQLGEN_TRANSPILATION_MAKE_FIELD_HPP_
#define SQLGEN_TRANSPILATION_MAKE_FIELD_HPP_

#include <limits>
#include <rfl.hpp>
#include <type_traits>

#include "../Literal.hpp"
#include "../Result.hpp"
#include "../dynamic/SelectFrom.hpp"
#include "../parsing/Parser.hpp"
#include "Aggregation.hpp"
#include "AggregationOp.hpp"
#include "As.hpp"
#include "Col.hpp"
#include "Operation.hpp"
#include "Operator.hpp"
#include "OperatorCategory.hpp"
#include "Value.hpp"
#include "all_columns_exist.hpp"
#include "dynamic_aggregation_t.hpp"
#include "dynamic_operator_t.hpp"
#include "get_table_t.hpp"
#include "is_timestamp.hpp"
#include "remove_as_t.hpp"
#include "remove_nullable_t.hpp"
#include "to_alias.hpp"
#include "to_duration.hpp"
#include "to_value.hpp"
#include "underlying_t.hpp"

namespace sqlgen::transpilation {

template <class TableTupleType, class FieldType>
struct MakeField;

template <class TableTupleType, class T>
struct MakeField {
  static constexpr bool is_aggregation = false;
  static constexpr bool is_column = false;
  static constexpr bool is_operation = false;

  using Name = Nothing;
  using Type = std::remove_cvref_t<T>;

  dynamic::SelectFrom::Field operator()(const auto& _val) const {
    return dynamic::SelectFrom::Field{
        dynamic::Operation{.val = to_value(_val)}};
  }
};

template <class TableTupleType, rfl::internal::StringLiteral _name,
          rfl::internal::StringLiteral _alias>
struct MakeField<TableTupleType, Col<_name, _alias>> {
  static_assert(all_columns_exist<TableTupleType, Col<_name, _alias>>(),
                "A required column does not exist.");

  static constexpr bool is_aggregation = false;
  static constexpr bool is_column = true;
  static constexpr bool is_operation = false;

  using TableType =
      get_table_t<typename Col<_name, _alias>::Alias, TableTupleType>;

  using Name = Literal<_name>;
  using Type = rfl::field_type_t<_name, TableType>;

  dynamic::SelectFrom::Field operator()(const auto&) const {
    return dynamic::SelectFrom::Field{dynamic::Operation{
        .val = dynamic::Column{.alias = to_alias<Literal<_alias>>(),
                               .name = _name.str()}}};
  }
};

template <class TableTupleType, class T>
struct MakeField<TableTupleType, Value<T>> {
  static constexpr bool is_aggregation = false;
  static constexpr bool is_column = false;
  static constexpr bool is_operation = false;

  using Name = Nothing;
  using Type = std::remove_cvref_t<T>;

  dynamic::SelectFrom::Field operator()(const auto& _val) const {
    return dynamic::SelectFrom::Field{
        dynamic::Operation{.val = to_value(_val.val)}};
  }
};

template <class TableTupleType, class ValueType,
          rfl::internal::StringLiteral _new_name>
struct MakeField<TableTupleType, As<ValueType, _new_name>> {
  static constexpr bool is_aggregation =
      MakeField<TableTupleType, ValueType>::is_aggregation;
  static constexpr bool is_column =
      MakeField<TableTupleType, ValueType>::is_column;
  static constexpr bool is_operation =
      MakeField<TableTupleType, ValueType>::is_operation;

  using Name = Literal<_new_name>;
  using Type =
      typename MakeField<TableTupleType, std::remove_cvref_t<ValueType>>::Type;

  dynamic::SelectFrom::Field operator()(const auto& _as) const {
    return dynamic::SelectFrom::Field{
        .val =
            dynamic::Operation{
                .val =
                    MakeField<TableTupleType, std::remove_cvref_t<ValueType>>{}(
                        _as.val)
                        .val.val},
        .as = _new_name.str()};
  }
};

template <class TableTupleType, AggregationOp _agg, class ValueType>
struct MakeField<TableTupleType, Aggregation<_agg, ValueType>> {
  static_assert(
      std::is_integral_v<
          remove_nullable_t<underlying_t<TableTupleType, ValueType>>> ||
          std::is_floating_point_v<
              remove_nullable_t<underlying_t<TableTupleType, ValueType>>>,
      "Values inside the aggregation must be numerical.");

  // Recursively checks if a type contains any aggregations.
  template <class T>
  struct HasAggregations;

  // Case: No operation.
  template <class T>
    requires(!MakeField<TableTupleType, remove_as_t<T>>::is_operation)
  struct HasAggregations<T> {
    static constexpr bool value =
        MakeField<TableTupleType, remove_as_t<T>>::is_aggregation;
  };

  // Case: Is operations: Check all operands.
  template <class T>
    requires(MakeField<TableTupleType, remove_as_t<T>>::is_operation)
  struct HasAggregations<T> {
    static constexpr bool value = HasAggregations<
        typename MakeField<TableTupleType, remove_as_t<T>>::Operands>::value;
  };

  // Case: Is a set of operands from an operation..
  template <class... FieldTypes>
  struct HasAggregations<rfl::Tuple<FieldTypes...>> {
    static constexpr bool value =
        (false || ... || HasAggregations<FieldTypes>::value);
  };

  static_assert(!HasAggregations<remove_as_t<ValueType>>::value,
                "Nested aggregations are not allowed. Please restructure your "
                "query to avoid nested aggregations.");

  static constexpr bool is_aggregation = true;
  static constexpr bool is_column = false;
  static constexpr bool is_operation = false;

  using Name = Nothing;
  using Type =
      typename MakeField<TableTupleType, std::remove_cvref_t<ValueType>>::Type;

  dynamic::SelectFrom::Field operator()(const auto& _val) const {
    using DynamicAggregationType = dynamic_aggregation_t<_agg>;
    return dynamic::SelectFrom::Field{dynamic::Operation{
        .val = dynamic::Aggregation{DynamicAggregationType{
            .val = Ref<dynamic::Operation>::make(
                MakeField<TableTupleType, std::remove_cvref_t<ValueType>>{}(
                    _val.val)
                    .val)}}}};
  }
};

template <class TableTupleType, rfl::internal::StringLiteral _name,
          rfl::internal::StringLiteral _alias>
struct MakeField<TableTupleType,
                 Aggregation<AggregationOp::count, Col<_name, _alias>>> {
  static_assert(all_columns_exist<TableTupleType, Col<_name, _alias>>(),
                "A column required in the COUNT or COUNT_DISTINCT aggregation "
                "does not exist.");

  static constexpr bool is_aggregation = true;
  static constexpr bool is_column = true;
  static constexpr bool is_operation = false;

  using Name = Literal<_name>;
  using Type = size_t;

  dynamic::SelectFrom::Field operator()(const auto& _agg) const {
    return dynamic::SelectFrom::Field{dynamic::Operation{
        .val = dynamic::Aggregation{dynamic::Aggregation::Count{
            .val = dynamic::Column{.alias = to_alias<Literal<_alias>>(),
                                   .name = _name.str()},
            .distinct = _agg.distinct}}}};
  }
};

template <class TableTupleType>
struct MakeField<TableTupleType, Aggregation<AggregationOp::count, All>> {
  static constexpr bool is_aggregation = true;
  static constexpr bool is_column = true;
  static constexpr bool is_operation = false;

  using Name = Nothing;
  using Type = size_t;

  dynamic::SelectFrom::Field operator()(const auto&) const {
    return dynamic::SelectFrom::Field{dynamic::Operation{
        .val = dynamic::Aggregation{
            dynamic::Aggregation::Count{.val = std::nullopt, .distinct = false},
        }}};
  }
};

template <class TableTupleType, class Operand1Type, class TargetType>
struct MakeField<TableTupleType, Operation<Operator::cast, Operand1Type,
                                           TypeHolder<TargetType>>> {
  static constexpr bool is_aggregation = false;
  static constexpr bool is_column = false;
  static constexpr bool is_operation = true;

  using Name = Nothing;
  using Type = std::remove_cvref_t<TargetType>;
  using Operands = rfl::Tuple<Operand1Type>;

  dynamic::SelectFrom::Field operator()(const auto& _o) const {
    return dynamic::SelectFrom::Field{
        dynamic::Operation{dynamic::Operation::Cast{
            .op1 = Ref<dynamic::Operation>::make(
                MakeField<TableTupleType, std::remove_cvref_t<Operand1Type>>{}(
                    _o.operand1)
                    .val),
            .target_type =
                parsing::Parser<std::remove_cvref_t<TargetType>>::to_type()}}};
  }
};

template <class TableTupleType, class Operand1Type, class... DurationTypes>
struct MakeField<TableTupleType,
                 Operation<Operator::date_plus_duration, Operand1Type,
                           rfl::Tuple<DurationTypes...>>> {
  static constexpr bool is_aggregation = false;
  static constexpr bool is_column = false;
  static constexpr bool is_operation = true;

  using Name = Nothing;
  using Type = underlying_t<TableTupleType, std::remove_cvref_t<Operand1Type>>;
  using Operands = rfl::Tuple<Operand1Type, DurationTypes...>;

  static_assert(is_timestamp_v<remove_nullable_t<Type>>,
                "Must be a timestamp.");

  dynamic::SelectFrom::Field operator()(const auto& _o) const {
    return dynamic::SelectFrom::Field{
        dynamic::Operation{dynamic::Operation::DatePlusDuration{
            .date = Ref<dynamic::Operation>::make(
                MakeField<TableTupleType, std::remove_cvref_t<Operand1Type>>{}(
                    _o.operand1)
                    .val),
            .durations = rfl::apply(
                [](const auto&... _ops) {
                  return std::vector<dynamic::Duration>({to_duration(_ops)...});
                },
                _o.operand2)}}};
  }
};

template <class TableTupleType, Operator _op, class Operand1Type>
  requires((num_operands_v<_op>) == 1)
struct MakeField<TableTupleType, Operation<_op, Operand1Type>> {
  static constexpr bool is_aggregation = false;
  static constexpr bool is_column = false;
  static constexpr bool is_operation = true;

  using Name = Nothing;
  using Type = underlying_t<TableTupleType, Operation<_op, Operand1Type>>;
  using Operands = rfl::Tuple<Operand1Type>;

  dynamic::SelectFrom::Field operator()(const auto& _o) const {
    using DynamicOperatorType = dynamic_operator_t<_op>;
    return dynamic::SelectFrom::Field{dynamic::Operation{DynamicOperatorType{
        .op1 = Ref<dynamic::Operation>::make(
            MakeField<TableTupleType, std::remove_cvref_t<Operand1Type>>{}(
                _o.operand1)
                .val)}}};
  }
};

template <class TableTupleType, Operator _op, class Operand1Type,
          class Operand2Type>
  requires((num_operands_v<_op>) == 2)
struct MakeField<TableTupleType, Operation<_op, Operand1Type, Operand2Type>> {
  static constexpr bool is_aggregation = false;
  static constexpr bool is_column = false;
  static constexpr bool is_operation = true;

  using Name = Nothing;
  using Type =
      underlying_t<TableTupleType, Operation<_op, Operand1Type, Operand2Type>>;
  using Operands = rfl::Tuple<Operand1Type, Operand2Type>;

  dynamic::SelectFrom::Field operator()(const auto& _o) const {
    using DynamicOperatorType = dynamic_operator_t<_op>;
    return dynamic::SelectFrom::Field{dynamic::Operation{DynamicOperatorType{
        .op1 = Ref<dynamic::Operation>::make(
            MakeField<TableTupleType, std::remove_cvref_t<Operand1Type>>{}(
                _o.operand1)
                .val),
        .op2 = Ref<dynamic::Operation>::make(
            MakeField<TableTupleType, std::remove_cvref_t<Operand2Type>>{}(
                _o.operand2)
                .val)}}};
  }
};

template <class TableTupleType, Operator _op, class Operand1Type,
          class Operand2Type, class Operand3Type>
  requires((num_operands_v<_op>) == 3)
struct MakeField<TableTupleType,
                 Operation<_op, Operand1Type, Operand2Type, Operand3Type>> {
  static constexpr bool is_aggregation = false;
  static constexpr bool is_column = false;
  static constexpr bool is_operation = true;

  using Name = Nothing;
  using Type =
      underlying_t<TableTupleType, Operation<Operator::replace, Operand1Type,
                                             Operand2Type, Operand3Type>>;
  using Operands = rfl::Tuple<Operand1Type, Operand2Type, Operand3Type>;

  dynamic::SelectFrom::Field operator()(const auto& _o) const {
    return dynamic::SelectFrom::Field{
        dynamic::Operation{dynamic::Operation::Replace{
            .op1 = Ref<dynamic::Operation>::make(
                MakeField<TableTupleType, std::remove_cvref_t<Operand1Type>>{}(
                    _o.operand1)
                    .val),
            .op2 = Ref<dynamic::Operation>::make(
                MakeField<TableTupleType, std::remove_cvref_t<Operand2Type>>{}(
                    _o.operand2)
                    .val),
            .op3 = Ref<dynamic::Operation>::make(
                MakeField<TableTupleType, std::remove_cvref_t<Operand3Type>>{}(
                    _o.operand3)
                    .val)}}};
  }
};

template <class TableTupleType, Operator _op, class... OperandTypes>
  requires((num_operands_v<_op>) == std::numeric_limits<size_t>::max())
struct MakeField<TableTupleType, Operation<_op, rfl::Tuple<OperandTypes...>>> {
  static constexpr bool is_aggregation = false;
  static constexpr bool is_column = false;
  static constexpr bool is_operation = true;

  using Name = Nothing;
  using Type =
      underlying_t<TableTupleType, Operation<_op, rfl::Tuple<OperandTypes...>>>;
  using Operands = rfl::Tuple<OperandTypes...>;

  dynamic::SelectFrom::Field operator()(const auto& _o) const {
    using DynamicOperatorType = dynamic_operator_t<_op>;
    return dynamic::SelectFrom::Field{dynamic::Operation{DynamicOperatorType{
        .ops = rfl::apply(
            [](const auto&... _ops) {
              return std::vector<Ref<dynamic::Operation>>(
                  {Ref<dynamic::Operation>::make(
                      MakeField<TableTupleType,
                                std::remove_cvref_t<OperandTypes>>{}(_ops)
                          .val)...});
            },
            _o.operand1)}}};
  }
};

template <class TableTupleType, class ValueType>
inline dynamic::SelectFrom::Field make_field(const ValueType& _val) {
  return MakeField<std::remove_cvref_t<TableTupleType>,
                   std::remove_cvref_t<ValueType>>{}(_val);
}

}  // namespace sqlgen::transpilation

#endif
