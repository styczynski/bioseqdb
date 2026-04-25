#ifndef SQLGEN_TRANSPILATION_TO_JOIN_HPP_
#define SQLGEN_TRANSPILATION_TO_JOIN_HPP_

#include <optional>
#include <rfl.hpp>
#include <type_traits>
#include <vector>

#include "../dynamic/Join.hpp"
#include "../dynamic/Table.hpp"
#include "Join.hpp"
#include "TableWrapper.hpp"
#include "get_schema.hpp"
#include "get_tablename.hpp"
#include "to_condition.hpp"

namespace sqlgen::transpilation {

template <class TableTupleType, class T>
struct ToJoin;

template <class TableTupleType, class QueryType>
struct ToJoin {
  template <class ConditionType, rfl::internal::StringLiteral _alias,
            JoinType _how>
  dynamic::Join operator()(const transpilation::Join<QueryType, ConditionType,
                                                     _alias, _how>& _join) {
    return dynamic::Join{
        .how = _how,
        .table_or_query = to_table_or_query(_join.table_or_query),
        .alias = Literal<_alias>().str(),
        .on = to_condition<TableTupleType>(_join.on)};
  }
};

template <class TableTupleType, class TableType>
struct ToJoin<TableTupleType, TableWrapper<TableType>> {
  template <class ConditionType, rfl::internal::StringLiteral _alias,
            JoinType _how>
  dynamic::Join operator()(
      const Join<TableWrapper<TableType>, ConditionType, _alias, _how>& _join) {
    using T = std::remove_cvref_t<TableType>;
    using Alias = typename Join<TableWrapper<TableType>, ConditionType, _alias,
                                _how>::Alias;

    return dynamic::Join{
        .how = _how,
        .table_or_query = dynamic::Table{.alias = std::nullopt,
                                         .name = get_tablename<T>(),
                                         .schema = get_schema<T>()},
        .alias = Alias().str(),
        .on = to_condition<TableTupleType>(_join.on)};
  }
};

template <class TableTupleType, class T, class ConditionType,
          rfl::internal::StringLiteral _alias, JoinType _how>
dynamic::Join to_join(const Join<T, ConditionType, _alias, _how>& _join) {
  return ToJoin<TableTupleType, T>{}(_join);
}

template <class TableTupleType, class... JoinTypes>
std::optional<std::vector<dynamic::Join>> to_joins(
    const rfl::Tuple<JoinTypes...>& _joins) {
  return rfl::apply(
      [](const auto&... _js) {
        return std::vector<dynamic::Join>({to_join<TableTupleType>(_js)...});
      },
      _joins);
}

template <class TableTupleType>
inline std::optional<std::vector<dynamic::Join>> to_joins(const Nothing&) {
  return std::nullopt;
}

}  // namespace sqlgen::transpilation

#endif
