#ifndef SQLGEN_TRANSPILATION_TO_SELECT_FROM_HPP_
#define SQLGEN_TRANSPILATION_TO_SELECT_FROM_HPP_

#include <ranges>
#include <rfl.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Result.hpp"
#include "../dynamic/ColumnOrAggregation.hpp"
#include "../dynamic/Join.hpp"
#include "../dynamic/SelectFrom.hpp"
#include "../dynamic/Table.hpp"
#include "../internal/collect/vector.hpp"
#include "Join.hpp"
#include "TableWrapper.hpp"
#include "check_aggregations.hpp"
#include "flatten_fields_t.hpp"
#include "get_table_t.hpp"
#include "make_fields.hpp"
#include "table_tuple_t.hpp"
#include "to_alias.hpp"
#include "to_condition.hpp"
#include "to_group_by.hpp"
#include "to_joins.hpp"
#include "to_limit.hpp"
#include "to_offset.hpp"
#include "to_order_by.hpp"
#include "to_table_or_query.hpp"

namespace sqlgen::transpilation {

template <class AliasT, class FieldsT, class TableOrQueryT, class JoinsT,
          class WhereT, class GroupByT, class OrderByT, class LimitT, class OffsetT>
struct SelectFromTypes {
  using AliasType = AliasT;
  using FieldsType = FieldsT;
  using TableOrQueryType = TableOrQueryT;
  using JoinsType = JoinsT;
  using WhereType = WhereT;
  using GroupByType = GroupByT;
  using OrderByType = OrderByT;
  using LimitType = LimitT;
  using OffsetType = OffsetT;

  using TableTupleType = table_tuple_t<TableOrQueryType, AliasType, JoinsType>;
};

template <class SelectFromT>
dynamic::SelectFrom to_select_from(const auto& _fields,
                                   const auto& _table_or_query,
                                   const auto& _joins, const auto& _where,
                                   const auto& _limit, const auto& _offset) {
  using TableTupleType = typename SelectFromT::TableTupleType;
  using AliasType = typename SelectFromT::AliasType;
  using FieldsType = typename SelectFromT::FieldsType;
  using GroupByType = typename SelectFromT::GroupByType;
  using OrderByType = typename SelectFromT::OrderByType;

  static_assert(check_aggregations<TableTupleType,
                                   flatten_fields_t<TableTupleType, FieldsType>,
                                   GroupByType>(),
                "The aggregations were not set up correctly. Please check the "
                "trace for a more detailed error message.");

  const auto fields = make_fields<TableTupleType, FieldsType>(
      _fields,
      std::make_integer_sequence<int, rfl::tuple_size_v<FieldsType>>());

  return dynamic::SelectFrom{
      .table_or_query = to_table_or_query(_table_or_query),
      .fields = fields,
      .alias = to_alias<AliasType>(),
      .joins = to_joins<TableTupleType>(_joins),
      .where = to_condition<std::remove_cvref_t<TableTupleType>>(_where),
      .group_by = to_group_by<GroupByType>(),
      .order_by = to_order_by<OrderByType>(),
      .limit = to_limit(_limit),
      .offset = to_offset(_offset)};
}

}  // namespace sqlgen::transpilation

#endif
