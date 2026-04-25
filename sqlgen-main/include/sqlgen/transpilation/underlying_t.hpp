#ifndef SQLGEN_TRANSPILATION_UNDERLYINGT_HPP_
#define SQLGEN_TRANSPILATION_UNDERLYINGT_HPP_

#include <rfl.hpp>
#include <string>
#include <type_traits>

#include "Aggregation.hpp"
#include "AggregationOp.hpp"
#include "Col.hpp"
#include "Desc.hpp"
#include "Operation.hpp"
#include "Value.hpp"
#include "all_columns_exist.hpp"
#include "dynamic_operator_t.hpp"
#include "get_table_t.hpp"
#include "is_nullable.hpp"
#include "is_timestamp.hpp"
#include "remove_nullable_t.hpp"
#include "remove_reflection_t.hpp"

namespace sqlgen::transpilation {

template <class TableTupleType, class _Type>
struct Underlying;

template <class TableTupleType, AggregationOp _op, class ValueType>
struct Underlying<TableTupleType, Aggregation<_op, ValueType>> {
  using Type =
      typename Underlying<TableTupleType, std::remove_cvref_t<ValueType>>::Type;
};

template <class TableTupleType, rfl::internal::StringLiteral _name,
          rfl::internal::StringLiteral _alias>
struct Underlying<TableTupleType, Col<_name, _alias>> {
  static_assert(all_columns_exist<TableTupleType, Col<_name, _alias>>(),
                "At least one column referenced in your query does not exist.");
  using Type = remove_reflection_t<
      rfl::field_type_t<_name, get_table_t<Literal<_alias>, TableTupleType>>>;
};

template <class TableTupleType, rfl::internal::StringLiteral _name,
          rfl::internal::StringLiteral _alias>
struct Underlying<TableTupleType, Desc<Col<_name, _alias>>> {
  using Type = remove_reflection_t<
      rfl::field_type_t<_name, get_table_t<Literal<_alias>, TableTupleType>>>;
};

template <class TableTupleType, class Operand1Type, class TargetType>
struct Underlying<TableTupleType, Operation<Operator::cast, Operand1Type,
                                            TypeHolder<TargetType>>> {
  using Type = std::conditional_t<
      is_nullable_v<typename Underlying<
          TableTupleType, std::remove_cvref_t<Operand1Type>>::Type>,
      std::optional<std::remove_cvref_t<TargetType>>,
      std::remove_cvref_t<TargetType>>;
};

template <class TableTupleType, class Head, class... Tail>
struct Underlying<TableTupleType,
                  Operation<Operator::coalesce, rfl::Tuple<Head, Tail...>>> {
  using Operand1Type =
      typename Underlying<TableTupleType, std::remove_cvref_t<Head>>::Type;

  static_assert(
      (true && ... &&
       std::is_same_v<remove_nullable_t<Operand1Type>,
                      remove_nullable_t<typename Underlying<
                          TableTupleType, std::remove_cvref_t<Tail>>::Type>>),
      "All inputs into coalesce(...) must have the same type.");

  using Type = std::conditional_t<
      (is_nullable_v<Operand1Type> && ... &&
       is_nullable_v<typename Underlying<TableTupleType,
                                         std::remove_cvref_t<Tail>>::Type>),
      std::optional<remove_nullable_t<Operand1Type>>,
      remove_nullable_t<Operand1Type>>;
};

template <class TableTupleType, class... OperandTypes>
struct Underlying<TableTupleType,
                  Operation<Operator::concat, rfl::Tuple<OperandTypes...>>> {
  static_assert(
      (true && ... &&
       std::is_same_v<
           remove_nullable_t<typename Underlying<
               TableTupleType, std::remove_cvref_t<OperandTypes>>::Type>,
           std::string>),
      "Must be a string");

  using Type = std::conditional_t<
      (false || ... ||
       is_nullable_v<typename Underlying<
           TableTupleType, std::remove_cvref_t<OperandTypes>>::Type>),
      std::optional<std::string>, std::string>;
};

template <class TableTupleType, class Operand1Type, class... DurationTypes>
struct Underlying<TableTupleType,
                  Operation<Operator::date_plus_duration, Operand1Type,
                            rfl::Tuple<DurationTypes...>>> {
  using Underlying1 = typename Underlying<TableTupleType, Operand1Type>::Type;

  static_assert(is_timestamp_v<remove_nullable_t<Underlying1>>,
                "Must be a timestamp");

  using Type = std::conditional_t<is_nullable_v<Underlying1>,
                                  std::optional<remove_nullable_t<Underlying1>>,
                                  Underlying1>;
};

template <class TableTupleType, class Operand1Type, class Operand2Type>
struct Underlying<TableTupleType, Operation<Operator::days_between,
                                            Operand1Type, Operand2Type>> {
  using Underlying1 = typename Underlying<TableTupleType, Operand1Type>::Type;
  using Underlying2 = typename Underlying<TableTupleType, Operand2Type>::Type;

  static_assert(is_timestamp_v<remove_nullable_t<Underlying1>>,
                "Must be a timestamp");
  static_assert(is_timestamp_v<remove_nullable_t<Underlying2>>,
                "Must be a timestamp");

  using Type = std::conditional_t<is_nullable_v<Underlying1> ||
                                      is_nullable_v<Underlying2>,
                                  std::optional<double>, double>;
};

template <class TableTupleType, class Operand1Type, class Operand2Type,
          class Operand3Type>
struct Underlying<TableTupleType, Operation<Operator::replace, Operand1Type,
                                            Operand2Type, Operand3Type>> {
  using Underlying1 =
      typename Underlying<TableTupleType,
                          std::remove_cvref_t<Operand1Type>>::Type;
  using Underlying2 =
      typename Underlying<TableTupleType,
                          std::remove_cvref_t<Operand2Type>>::Type;
  using Underlying3 =
      typename Underlying<TableTupleType,
                          std::remove_cvref_t<Operand3Type>>::Type;

  static_assert(std::is_same_v<remove_nullable_t<Underlying1>, std::string>,
                "Must be a string");
  static_assert(std::is_same_v<remove_nullable_t<Underlying2>, std::string>,
                "Must be a string");
  static_assert(std::is_same_v<remove_nullable_t<Underlying3>, std::string>,
                "Must be a string");

  using Type = std::conditional_t<is_nullable_v<Underlying1> ||
                                      is_nullable_v<Underlying2> ||
                                      is_nullable_v<Underlying3>,
                                  std::optional<std::string>, std::string>;
};

template <class TableTupleType, class Operand1Type, class Operand2Type>
struct Underlying<TableTupleType,
                  Operation<Operator::round, Operand1Type, Operand2Type>> {
  using Underlying1 =
      typename Underlying<TableTupleType,
                          std::remove_cvref_t<Operand1Type>>::Type;
  using Underlying2 =
      typename Underlying<TableTupleType,
                          std::remove_cvref_t<Operand2Type>>::Type;

  static_assert(std::is_integral_v<remove_nullable_t<Underlying1>> ||
                    std::is_floating_point_v<remove_nullable_t<Underlying1>>,
                "Must be a numerical type");
  static_assert(std::is_integral_v<Underlying2>, "Must be an integral type");

  using Type = Underlying1;
};

template <class TableTupleType, class Operand1Type>
struct Underlying<TableTupleType,
                  Operation<Operator::unixepoch, Operand1Type>> {
  using Underlying1 = typename Underlying<TableTupleType, Operand1Type>::Type;

  static_assert(is_timestamp_v<remove_nullable_t<Underlying1>>,
                "Must be a timestamp");

  using Type = std::conditional_t<is_nullable_v<Underlying1>,
                                  std::optional<time_t>, time_t>;
};

template <class TableTupleType, Operator _op, class Operand1Type>
  requires((num_operands_v<_op>) == 1 &&
           (operator_category_v<_op>) == OperatorCategory::date_part)
struct Underlying<TableTupleType, Operation<_op, Operand1Type>> {
  using Underlying1 =
      typename Underlying<TableTupleType,
                          std::remove_cvref_t<Operand1Type>>::Type;

  static_assert(is_timestamp_v<remove_nullable_t<Underlying1>>,
                "Must be a timestamp");

  using Type =
      std::conditional_t<is_nullable_v<Underlying1>, std::optional<int>, int>;
};

template <class TableTupleType, Operator _op, class Operand1Type>
  requires((num_operands_v<_op>) == 1 &&
           (operator_category_v<_op>) == OperatorCategory::string)
struct Underlying<TableTupleType, Operation<_op, Operand1Type>> {
  using Underlying1 =
      typename Underlying<TableTupleType,
                          std::remove_cvref_t<Operand1Type>>::Type;

  static_assert(std::is_same_v<remove_nullable_t<Underlying1>, std::string>,
                "Must be a string");

  using StringType =
      std::conditional_t<is_nullable_v<Underlying1>, std::optional<std::string>,
                         std::string>;
  using SizeType = std::conditional_t<is_nullable_v<Underlying1>,
                                      std::optional<size_t>, size_t>;
  using Type =
      std::conditional_t<_op == Operator::length, SizeType, StringType>;
};

template <class TableTupleType, Operator _op, class Operand1Type,
          class Operand2Type>
  requires((num_operands_v<_op>) == 2 &&
           (operator_category_v<_op>) == OperatorCategory::string)
struct Underlying<TableTupleType, Operation<_op, Operand1Type, Operand2Type>> {
  using Underlying1 =
      typename Underlying<TableTupleType,
                          std::remove_cvref_t<Operand1Type>>::Type;
  using Underlying2 =
      typename Underlying<TableTupleType,
                          std::remove_cvref_t<Operand2Type>>::Type;

  static_assert(std::is_same_v<remove_nullable_t<Underlying1>, std::string>,
                "Must be a string");
  static_assert(std::is_same_v<remove_nullable_t<Underlying2>, std::string>,
                "Must be a string");

  using Type = std::conditional_t<is_nullable_v<Underlying1> ||
                                      is_nullable_v<Underlying2>,
                                  std::optional<std::string>, std::string>;
};

template <class TableTupleType, Operator _op, class Operand1Type>
  requires((num_operands_v<_op>) == 1 &&
           (operator_category_v<_op>) == OperatorCategory::numerical)
struct Underlying<TableTupleType, Operation<_op, Operand1Type>> {
  using Underlying1 =
      typename Underlying<TableTupleType,
                          std::remove_cvref_t<Operand1Type>>::Type;

  static_assert(std::is_integral_v<remove_nullable_t<Underlying1>> ||
                    std::is_floating_point_v<remove_nullable_t<Underlying1>>,
                "Must be a numerical type");

  using Type = Underlying1;
};

template <class TableTupleType, Operator _op, class Operand1Type,
          class Operand2Type>
  requires((num_operands_v<_op>) == 2 &&
           (operator_category_v<_op>) == OperatorCategory::numerical)
struct Underlying<TableTupleType, Operation<_op, Operand1Type, Operand2Type>> {
  using Underlying1 =
      typename Underlying<TableTupleType,
                          std::remove_cvref_t<Operand1Type>>::Type;
  using Underlying2 =
      typename Underlying<TableTupleType,
                          std::remove_cvref_t<Operand2Type>>::Type;

  static_assert(
      requires(remove_nullable_t<Underlying1> op1,
               remove_nullable_t<Underlying2> op2) { op1 + op2; },
      "Binary operations are not possible on these types.");

  using ResultType = std::invoke_result_t<
      decltype([](const auto& op1, const auto& op2) { return op1 + op2; }),
      remove_nullable_t<Underlying1>, remove_nullable_t<Underlying2>>;

  using Type = std::conditional_t<is_nullable_v<Underlying1> ||
                                      is_nullable_v<Underlying2>,
                                  std::optional<ResultType>, ResultType>;
};

template <class TableTupleType, class _Type>
struct Underlying<TableTupleType, Value<_Type>> {
  using Type = remove_reflection_t<_Type>;
};

template <class TableTupleType, class T>
using underlying_t = typename Underlying<std::remove_cvref_t<TableTupleType>,
                                         std::remove_cvref_t<T>>::Type;

}  // namespace sqlgen::transpilation

#endif
