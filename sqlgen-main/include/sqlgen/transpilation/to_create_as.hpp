#ifndef SQLGEN_TRANSPILATION_TO_CREATE_AS_HPP_
#define SQLGEN_TRANSPILATION_TO_CREATE_AS_HPP_

#include <rfl.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../dynamic/CreateAs.hpp"
#include "../dynamic/Table.hpp"
#include "get_schema.hpp"
#include "get_tablename.hpp"
#include "to_select_from.hpp"

namespace sqlgen::transpilation {

template <class T, class TableTupleType, class AliasType, class FieldsType,
          class TableOrQueryType, class JoinsType, class WhereType,
          class GroupByType, class OrderByType, class LimitType, class OffsetType>
  requires std::is_class_v<std::remove_cvref_t<T>> &&
           std::is_aggregate_v<std::remove_cvref_t<T>>
dynamic::CreateAs to_create_as(const dynamic::CreateAs::What _what,
                               const bool _or_replace,
                               const bool _if_not_exists,
                               const FieldsType& _fields,
                               const TableOrQueryType& _table_or_query,
                               const JoinsType& _joins, const WhereType& _where,
                               const LimitType& _limit, const OffsetType& _offset) {
  using SelectFromTypes =
      transpilation::SelectFromTypes<AliasType, FieldsType, TableOrQueryType,
                                     JoinsType, WhereType, GroupByType,
                                     OrderByType, LimitType, OffsetType>;

  return dynamic::CreateAs{
      .what = _what,
      .table_or_view = dynamic::Table{.alias = std::nullopt,
                                      .name = get_tablename<T>(),
                                      .schema = get_schema<T>()},
      .query = to_select_from<SelectFromTypes>(_fields, _table_or_query, _joins,
                                               _where, _limit, _offset),
      .or_replace = _or_replace,
      .if_not_exists = _if_not_exists};
}

}  // namespace sqlgen::transpilation

#endif
