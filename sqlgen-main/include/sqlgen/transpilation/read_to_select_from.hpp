#ifndef SQLGEN_TRANSPILATION_READ_TO_SELECT_FROM_HPP_
#define SQLGEN_TRANSPILATION_READ_TO_SELECT_FROM_HPP_

#include <ranges>
#include <rfl.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Result.hpp"
#include "../dynamic/ColumnOrAggregation.hpp"
#include "../dynamic/SelectFrom.hpp"
#include "../dynamic/Table.hpp"
#include "../internal/collect/vector.hpp"
#include "get_schema.hpp"
#include "get_tablename.hpp"
#include "make_columns.hpp"
#include "to_condition.hpp"
#include "to_limit.hpp"
#include "to_offset.hpp"
#include "to_order_by.hpp"

namespace sqlgen::transpilation {

template <class T, class WhereType = Nothing, class OrderByType = Nothing,
          class LimitType = Nothing, class OffsetType = Nothing>
  requires std::is_class_v<std::remove_cvref_t<T>> &&
           std::is_aggregate_v<std::remove_cvref_t<T>>
dynamic::SelectFrom read_to_select_from(const WhereType& _where = WhereType{},
                                        const LimitType& _limit = LimitType{},
                                        const OffsetType& _offset = OffsetType{}) {
  using namespace std::ranges::views;

  using NamedTupleType = rfl::named_tuple_t<std::remove_cvref_t<T>>;
  using Fields = typename NamedTupleType::Fields;

  const auto columns = make_columns<Fields>(
      std::make_integer_sequence<int, rfl::tuple_size_v<Fields>>());

  const auto fields = sqlgen::internal::collect::vector(
      columns | transform([](const auto& _col) -> dynamic::SelectFrom::Field {
        return dynamic::SelectFrom::Field{.val = _col};
      }));

  return dynamic::SelectFrom{
      .table_or_query = dynamic::Table{.alias = std::nullopt,
                                       .name = get_tablename<T>(),
                                       .schema = get_schema<T>()},
      .fields = fields,
      .where = to_condition<std::remove_cvref_t<T>>(_where),
      .order_by = to_order_by<OrderByType>(),
      .limit = to_limit(_limit),
      .offset = to_offset(_offset)};
}

}  // namespace sqlgen::transpilation

#endif
