#ifndef SQLGEN_TRANSPILATION_TO_SQL_HPP_
#define SQLGEN_TRANSPILATION_TO_SQL_HPP_

#include <vector>

#include "../create_as.hpp"
#include "../create_index.hpp"
#include "../create_table.hpp"
#include "../delete_from.hpp"
#include "../drop.hpp"
#include "../dynamic/Statement.hpp"
#include "../insert.hpp"
#include "../read.hpp"
#include "../select_from.hpp"
#include "../unite.hpp"
#include "../update.hpp"
#include "columns_t.hpp"
#include "read_to_select_from.hpp"
#include "table_tuple_t.hpp"
#include "to_create_index.hpp"
#include "to_create_table.hpp"
#include "to_delete_from.hpp"
#include "to_drop.hpp"
#include "to_insert_or_write.hpp"
#include "to_select_from.hpp"
#include "to_union.hpp"
#include "to_update.hpp"
#include "value_t.hpp"

namespace sqlgen::transpilation {

template <class T>
struct ToSQL;

template <class ValueType, class TableOrQueryType, class AliasType,
          class FieldsType, class JoinsType, class WhereType, class GroupByType,
          class OrderByType, class LimitType, class OffsetType, class ToType>
struct ToSQL<
    CreateAs<ValueType, TableOrQueryType, AliasType, FieldsType, JoinsType,
             WhereType, GroupByType, OrderByType, LimitType, OffsetType, ToType>> {
  dynamic::Statement operator()(const auto& _create_as) const {
    using TableTupleType =
        transpilation::table_tuple_t<ValueType, AliasType, JoinsType>;
    return to_create_as<ValueType, TableTupleType, AliasType, FieldsType,
                        TableOrQueryType, JoinsType, WhereType, GroupByType,
                        OrderByType, LimitType>(
        _create_as.what_, _create_as.or_replace_, _create_as.if_not_exists_,
        _create_as.as_.fields_, _create_as.as_.from_, _create_as.as_.joins_,
        _create_as.as_.where_, _create_as.as_.limit_, _create_as.as_.offset_);
  }
};

template <rfl::internal::StringLiteral _name, class ValueType, class WhereType,
          class... ColTypes>
struct ToSQL<CreateIndex<_name, ValueType, WhereType, ColTypes...>> {
  dynamic::Statement operator()(const auto& _create_index) const {
    return transpilation::to_create_index<
        ValueType, columns_t<ValueType, typename ColTypes::ColType...>,
        WhereType>(_name.str(), _create_index.unique_,
                   _create_index.if_not_exists_, _create_index.where_);
  }
};

template <class T>
struct ToSQL<CreateTable<T>> {
  dynamic::Statement operator()(const auto&) const {
    return to_create_table<T>();
  }
};

template <class T, class WhereType>
struct ToSQL<DeleteFrom<T, WhereType>> {
  dynamic::Statement operator()(const auto& _delete_from) const {
    return to_delete_from<T>(_delete_from.where_);
  }
};

template <class T>
struct ToSQL<Drop<T>> {
  dynamic::Statement operator()(const auto& _drop) const {
    return to_drop<T>(_drop.what_, _drop.if_exists_, _drop.cascade_);
  }
};

template <class T>
struct ToSQL<Insert<T>> {
  dynamic::Statement operator()(const auto&) const {
    return to_insert_or_write<T, dynamic::Insert>();
  }
};

template <class ContainerType, class WhereType, class OrderByType,
          class LimitType, class OffsetType>
struct ToSQL<Read<ContainerType, WhereType, OrderByType, LimitType, OffsetType>> {
  dynamic::Statement operator()(const auto& _read) const {
    return read_to_select_from<value_t<ContainerType>, WhereType, OrderByType,
                               LimitType, OffsetType>(_read.where_, _read.limit_, _read.offset_);
  }
};

template <class... Ts>
struct ToSQL<SelectFrom<Ts...>> {
  dynamic::Statement operator()(const auto& _select_from) const {
    using SelectFromTypes = typename SelectFrom<Ts...>::SelectFromTypes;
    return to_select_from<SelectFromTypes>(
        _select_from.fields_, _select_from.from_, _select_from.joins_,
        _select_from.where_, _select_from.limit_, _select_from.offset_);
  }
};

template <class T, class SetsType, class WhereType>
struct ToSQL<Update<T, SetsType, WhereType>> {
  dynamic::Statement operator()(const auto& _update) const {
    return to_update<T, SetsType, WhereType>(_update.sets_, _update.where_);
  }
};

template <class ContainerType, class... Selects>
struct ToSQL<sqlgen::Union<ContainerType, Selects...>> {
  dynamic::Statement operator()(const auto& _union) const {
    return to_union<ContainerType>(_union.selects_, _union.all_);
  }
};

template <class T>
dynamic::Statement to_sql(const T& _t) {
  return ToSQL<std::remove_cvref_t<T>>{}(_t);
}

}  // namespace sqlgen::transpilation

#endif
