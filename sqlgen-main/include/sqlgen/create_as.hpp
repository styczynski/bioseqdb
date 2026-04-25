#ifndef SQLGEN_CREATEAS_HPP_
#define SQLGEN_CREATEAS_HPP_

#include <concepts>
#include <rfl.hpp>
#include <type_traits>

#include "Result.hpp"
#include "dynamic/CreateAs.hpp"
#include "is_connection.hpp"
#include "select_from.hpp"
#include "transpilation/extract_table_t.hpp"
#include "transpilation/get_table_or_view.hpp"
#include "transpilation/table_tuple_t.hpp"
#include "transpilation/to_create_as.hpp"

namespace sqlgen {

template <class ValueType, class TableOrQueryType, class AliasType,
          class FieldsType, class JoinsType, class WhereType, class GroupByType,
          class OrderByType, class LimitType, class OffsetType, class ToType, class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> create_as_impl(
    const Ref<Connection>& _conn, const dynamic::CreateAs::What _what,
    const SelectFrom<TableOrQueryType, AliasType, FieldsType, JoinsType,
                     WhereType, GroupByType, OrderByType, LimitType, OffsetType, ToType>&
        _as,
    const bool _or_replace, const bool _if_not_exists) {
  using TableTupleType =
      transpilation::table_tuple_t<TableOrQueryType, AliasType, JoinsType>;

  const auto query = transpilation::to_create_as<
      ValueType, TableTupleType, AliasType, FieldsType, TableOrQueryType,
      JoinsType, WhereType, GroupByType, OrderByType, LimitType, OffsetType>(
      _what, _or_replace, _if_not_exists, _as.fields_, _as.from_, _as.joins_,
      _as.where_, _as.limit_, _as.offset_);

  return _conn->execute(_conn->to_sql(query)).transform([&](const auto&) {
    return _conn;
  });
}

template <class ValueType, class TableOrQueryType, class AliasType,
          class FieldsType, class JoinsType, class WhereType, class GroupByType,
          class OrderByType, class LimitType, class OffsetType, class ToType, class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> create_as_impl(
    const Result<Ref<Connection>>& _res, const dynamic::CreateAs::What _what,
    const SelectFrom<TableOrQueryType, AliasType, FieldsType, JoinsType,
                     WhereType, GroupByType, OrderByType, LimitType, OffsetType, ToType>&
        _as,
    const bool _or_replace, const bool _if_not_exists) {
  return _res.and_then([&](const auto& _conn) {
    return create_as_impl<ValueType>(_conn, _what, _as, _or_replace,
                                     _if_not_exists);
  });
}

template <class ValueType, class TableOrQueryType, class AliasType,
          class FieldsType, class JoinsType, class WhereType, class GroupByType,
          class OrderByType, class LimitType, class OffsetType, class ToType>
struct CreateAs {
  using As = SelectFrom<TableOrQueryType, AliasType, FieldsType, JoinsType,
                        WhereType, GroupByType, OrderByType, LimitType, OffsetType, ToType>;

  static_assert(
      requires(transpilation::extract_table_t<As, false> a) {
        { rfl::from_named_tuple<ValueType>(a) } -> std::same_as<ValueType>;
      },
      "The query inserted into create_..._as must be convertible to "
      "the output struct.");

  auto operator()(const auto& _conn) const {
    return create_as_impl<ValueType>(_conn, what_, as_, or_replace_,
                                     if_not_exists_);
  }

  dynamic::CreateAs::What what_;
  As as_;
  bool or_replace_;
  bool if_not_exists_;
};

template <class ValueType, class... Args>
inline auto create_as(const SelectFrom<Args...>& _as) {
  return CreateAs<std::remove_cvref_t<ValueType>, Args...>{
      .what_ = transpilation::get_table_or_view<ValueType>(),
      .as_ = _as,
      .or_replace_ = false,
      .if_not_exists_ = false};
}

template <class ValueType, class... Args>
inline auto create_or_replace_view_as(const SelectFrom<Args...>& _as) {
  static_assert(
      transpilation::get_table_or_view<ValueType>() ==
          dynamic::TableOrView::view,
      "Only views can be created using create_or_replace_view_as(...), "
      "not tables. To declare a struct a view, "
      "simply add 'static constexpr bool is_view = true;' to the struct "
      "declaration.");
  return CreateAs<std::remove_cvref_t<ValueType>, Args...>{
      .what_ = dynamic::TableOrView::view,
      .as_ = _as,
      .or_replace_ = true,
      .if_not_exists_ = false};
}
};  // namespace sqlgen

#endif
