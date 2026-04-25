#ifndef SQLGEN_TRANSPILATION_CHECKAGGREGATIONS_HPP_
#define SQLGEN_TRANSPILATION_CHECKAGGREGATIONS_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "../Literal.hpp"
#include "Col.hpp"
#include "group_by_t.hpp"
#include "make_field.hpp"
#include "remove_as_t.hpp"

namespace sqlgen::transpilation {

template <class TablesType, class FieldsType, class GroupByType>
struct CheckAggregation;

/// Case: No aggregation, no group by.
template <class TablesType, class... FieldTypes>
  requires(true && ... && !MakeField<TablesType, FieldTypes>::is_aggregation)
struct CheckAggregation<TablesType, rfl::Tuple<FieldTypes...>, Nothing> {
  static constexpr bool value = true;
};

/// Case: At least one aggregation, no group by.
template <class TablesType, class... FieldTypes>
  requires(false || ... || MakeField<TablesType, FieldTypes>::is_aggregation)
struct CheckAggregation<TablesType, rfl::Tuple<FieldTypes...>, Nothing> {
  static constexpr bool value =
      (true && ... &&
       (MakeField<TablesType, FieldTypes>::is_aggregation ||
        !MakeField<TablesType, FieldTypes>::is_column));
  static_assert(
      value,
      "If any column is aggregated and there is no GROUP BY, then all columns "
      "must be aggregated.");
};

/// Case: There is a group by.
template <class TablesType, class... FieldTypes, class... ColTypes>
struct CheckAggregation<TablesType, rfl::Tuple<FieldTypes...>,
                        GroupBy<ColTypes...>> {
  template <class F>
  static constexpr bool included_in_group_by =
      (false || ... ||
       std::is_same_v<std::remove_cvref_t<remove_as_t<F>>,
                      std::remove_cvref_t<ColTypes>>);

  static constexpr bool value =
      (true && ... &&
       (MakeField<TablesType, FieldTypes>::is_aggregation ||
        (!MakeField<TablesType, FieldTypes>::is_column &&
         !MakeField<TablesType, FieldTypes>::is_operation) ||
        included_in_group_by<FieldTypes>));

  static_assert(value,
                "If there is a GROUP BY, then all columns "
                "must either be aggregated or included inside the GROUP BY.");
};

template <class TablesType, class FieldsType, class GroupByType>
consteval bool check_aggregations() {
  return CheckAggregation<std::remove_cvref_t<TablesType>,
                          std::remove_cvref_t<FieldsType>,
                          std::remove_cvref_t<GroupByType>>::value;
}

}  // namespace sqlgen::transpilation

#endif
