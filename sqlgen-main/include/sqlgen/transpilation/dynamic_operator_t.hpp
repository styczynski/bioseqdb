#ifndef SQLGEN_TRANSPILATION_DYNAMICOPERATORT_HPP_
#define SQLGEN_TRANSPILATION_DYNAMICOPERATORT_HPP_

#include <limits>

#include "../dynamic/Operation.hpp"
#include "Operator.hpp"
#include "OperatorCategory.hpp"

namespace sqlgen::transpilation {

template <Operator op>
struct DynamicOperator;

template <>
struct DynamicOperator<Operator::abs> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Abs;
};

template <>
struct DynamicOperator<Operator::cast> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::other;
  using Type = dynamic::Operation::Cast;
};

template <>
struct DynamicOperator<Operator::ceil> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Ceil;
};

template <>
struct DynamicOperator<Operator::coalesce> {
  static constexpr size_t num_operands = std::numeric_limits<size_t>::max();
  static constexpr auto category = OperatorCategory::other;
  using Type = dynamic::Operation::Coalesce;
};

template <>
struct DynamicOperator<Operator::concat> {
  static constexpr size_t num_operands = std::numeric_limits<size_t>::max();
  static constexpr auto category = OperatorCategory::string;
  using Type = dynamic::Operation::Concat;
};

template <>
struct DynamicOperator<Operator::cos> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Cos;
};

template <>
struct DynamicOperator<Operator::date_plus_duration> {
  static constexpr size_t num_operands = std::numeric_limits<size_t>::max();
  static constexpr auto category = OperatorCategory::other;
  using Type = dynamic::Operation::DatePlusDuration;
};

template <>
struct DynamicOperator<Operator::day> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::date_part;
  using Type = dynamic::Operation::Day;
};

template <>
struct DynamicOperator<Operator::days_between> {
  static constexpr size_t num_operands = 2;
  static constexpr auto category = OperatorCategory::other;
  using Type = dynamic::Operation::DaysBetween;
};

template <>
struct DynamicOperator<Operator::divides> {
  static constexpr size_t num_operands = 2;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Divides;
};

template <>
struct DynamicOperator<Operator::exp> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Exp;
};

template <>
struct DynamicOperator<Operator::floor> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Floor;
};

template <>
struct DynamicOperator<Operator::hour> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::date_part;
  using Type = dynamic::Operation::Hour;
};

template <>
struct DynamicOperator<Operator::length> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::string;
  using Type = dynamic::Operation::Length;
};

template <>
struct DynamicOperator<Operator::ln> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Ln;
};

template <>
struct DynamicOperator<Operator::log2> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Log2;
};

template <>
struct DynamicOperator<Operator::lower> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::string;
  using Type = dynamic::Operation::Lower;
};

template <>
struct DynamicOperator<Operator::ltrim> {
  static constexpr size_t num_operands = 2;
  static constexpr auto category = OperatorCategory::string;
  using Type = dynamic::Operation::LTrim;
};

template <>
struct DynamicOperator<Operator::minus> {
  static constexpr size_t num_operands = 2;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Minus;
};

template <>
struct DynamicOperator<Operator::minute> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::date_part;
  using Type = dynamic::Operation::Minute;
};

template <>
struct DynamicOperator<Operator::mod> {
  static constexpr size_t num_operands = 2;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Mod;
};

template <>
struct DynamicOperator<Operator::month> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::date_part;
  using Type = dynamic::Operation::Month;
};

template <>
struct DynamicOperator<Operator::multiplies> {
  static constexpr size_t num_operands = 2;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Multiplies;
};

template <>
struct DynamicOperator<Operator::plus> {
  static constexpr size_t num_operands = 2;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Plus;
};

template <>
struct DynamicOperator<Operator::replace> {
  static constexpr size_t num_operands = 3;
  static constexpr auto category = OperatorCategory::other;
  using Type = dynamic::Operation::Replace;
};

template <>
struct DynamicOperator<Operator::round> {
  static constexpr size_t num_operands = 2;
  static constexpr auto category = OperatorCategory::other;
  using Type = dynamic::Operation::Round;
};

template <>
struct DynamicOperator<Operator::rtrim> {
  static constexpr size_t num_operands = 2;
  static constexpr auto category = OperatorCategory::string;
  using Type = dynamic::Operation::RTrim;
};

template <>
struct DynamicOperator<Operator::second> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::date_part;
  using Type = dynamic::Operation::Second;
};

template <>
struct DynamicOperator<Operator::sin> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Sin;
};

template <>
struct DynamicOperator<Operator::sqrt> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Sqrt;
};

template <>
struct DynamicOperator<Operator::tan> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::numerical;
  using Type = dynamic::Operation::Tan;
};

template <>
struct DynamicOperator<Operator::trim> {
  static constexpr size_t num_operands = 2;
  static constexpr auto category = OperatorCategory::string;
  using Type = dynamic::Operation::Trim;
};

template <>
struct DynamicOperator<Operator::unixepoch> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::other;
  using Type = dynamic::Operation::Unixepoch;
};

template <>
struct DynamicOperator<Operator::upper> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::string;
  using Type = dynamic::Operation::Upper;
};

template <>
struct DynamicOperator<Operator::weekday> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::date_part;
  using Type = dynamic::Operation::Weekday;
};

template <>
struct DynamicOperator<Operator::year> {
  static constexpr size_t num_operands = 1;
  static constexpr auto category = OperatorCategory::date_part;
  using Type = dynamic::Operation::Year;
};

template <Operator op>
using dynamic_operator_t = typename DynamicOperator<op>::Type;

template <Operator op>
inline constexpr size_t num_operands_v = DynamicOperator<op>::num_operands;

template <Operator op>
inline constexpr auto operator_category_v = DynamicOperator<op>::category;

}  // namespace sqlgen::transpilation

#endif
