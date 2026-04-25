#ifndef SQLGEN_TRANSPILATION_ORDERBYT_HPP_
#define SQLGEN_TRANSPILATION_ORDERBYT_HPP_

#include <rfl.hpp>
#include <type_traits>
#include <vector>

#include "Col.hpp"
#include "Desc.hpp"
#include "all_columns_exist.hpp"
#include "check_aggregations.hpp"

namespace sqlgen::transpilation {

template <class _ColType>
struct OrderByWrapper;

template <rfl::internal::StringLiteral _name,
          rfl::internal::StringLiteral _alias>
struct OrderByWrapper<transpilation::Col<_name, _alias>> {
  using ColType = transpilation::Col<_name, _alias>;
  constexpr static bool desc = false;
};

template <rfl::internal::StringLiteral _name,
          rfl::internal::StringLiteral _alias>
struct OrderByWrapper<Desc<transpilation::Col<_name, _alias>>> {
  using ColType = transpilation::Col<_name, _alias>;
  constexpr static bool desc = true;
};

template <class... WrapperTypes>
struct OrderBy {};

template <class TablesType, class GroupByType, class... ColTypes>
auto make_order_by() {
  static_assert(
      all_columns_exist<TablesType,
                        typename OrderByWrapper<ColTypes>::ColType...>(),
      "A column in order_by does not exist.");
  static_assert(
      check_aggregations<
          TablesType, rfl::Tuple<typename OrderByWrapper<ColTypes>::ColType...>,
          GroupByType>(),
      "The columns in the ORDER BY clause have not been properly "
      "aggregated. Please refer to the stack trace for details.");

  return OrderBy<OrderByWrapper<ColTypes>...>{};
}

template <class TablesType, class GroupByType, class... ColTypes>
using order_by_t = std::invoke_result_t<
    decltype(make_order_by<TablesType, GroupByType,
                           typename ColTypes::ColType...>)>;

}  // namespace sqlgen::transpilation

#endif
