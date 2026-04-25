#ifndef SQLGEN_JOINS_HPP_
#define SQLGEN_JOINS_HPP_

#include <rfl.hpp>

#include "transpilation/Join.hpp"

namespace sqlgen {

template <rfl::internal::StringLiteral _alias, transpilation::JoinType _how,
          class TableOrQueryType, class ConditionType>
auto join(const TableOrQueryType& _table_or_query, const ConditionType& _on) {
  return transpilation::Join<TableOrQueryType, ConditionType, _alias, _how>{
      .table_or_query = _table_or_query, .on = _on};
};

template <rfl::internal::StringLiteral _alias, class QueryType,
          class ConditionType>
auto full_join(const QueryType& _query, const ConditionType& _on) {
  return join<_alias, transpilation::JoinType::full_join>(_query, _on);
}

template <class TableType, rfl::internal::StringLiteral _alias,
          class ConditionType>
auto full_join(const ConditionType& _on) {
  return join<_alias, transpilation::JoinType::full_join>(
      transpilation::TableWrapper<TableType>{}, _on);
}

template <rfl::internal::StringLiteral _alias, class QueryType,
          class ConditionType>
auto inner_join(const QueryType& _query, const ConditionType& _on) {
  return join<_alias, transpilation::JoinType::inner_join>(_query, _on);
}

template <class TableType, rfl::internal::StringLiteral _alias,
          class ConditionType>
auto inner_join(const ConditionType& _on) {
  return join<_alias, transpilation::JoinType::inner_join>(
      transpilation::TableWrapper<TableType>{}, _on);
}

template <rfl::internal::StringLiteral _alias, class QueryType,
          class ConditionType>
auto left_join(const QueryType& _query, const ConditionType& _on) {
  return join<_alias, transpilation::JoinType::left_join>(_query, _on);
}

template <class TableType, rfl::internal::StringLiteral _alias,
          class ConditionType>
auto left_join(const ConditionType& _on) {
  return join<_alias, transpilation::JoinType::left_join>(
      transpilation::TableWrapper<TableType>{}, _on);
}

template <rfl::internal::StringLiteral _alias, class QueryType,
          class ConditionType>
auto right_join(const QueryType& _query, const ConditionType& _on) {
  return join<_alias, transpilation::JoinType::right_join>(_query, _on);
}

template <class TableType, rfl::internal::StringLiteral _alias,
          class ConditionType>
auto right_join(const ConditionType& _on) {
  return join<_alias, transpilation::JoinType::right_join>(
      transpilation::TableWrapper<TableType>{}, _on);
}

}  // namespace sqlgen

#endif
