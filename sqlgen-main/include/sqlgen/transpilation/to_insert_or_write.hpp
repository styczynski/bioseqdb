#ifndef SQLGEN_TRANSPILATION_TO_INSERT_OR_WRITE_HPP_
#define SQLGEN_TRANSPILATION_TO_INSERT_OR_WRITE_HPP_

#include <ranges>
#include <rfl.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../dynamic/Insert.hpp"
#include "../dynamic/Table.hpp"
#include "../internal/collect/vector.hpp"
#include "../internal/remove_auto_incr_primary_t.hpp"
#include "get_schema.hpp"
#include "get_tablename.hpp"
#include "make_columns.hpp"

namespace sqlgen::transpilation {

template <class T, class InsertOrWrite>
  requires std::is_class_v<std::remove_cvref_t<T>> &&
           std::is_aggregate_v<std::remove_cvref_t<T>>
InsertOrWrite to_insert_or_write(bool or_replace) {
  using namespace std::ranges::views;

  using NamedTupleType = sqlgen::internal::remove_auto_incr_primary_t<
      rfl::named_tuple_t<std::remove_cvref_t<T>>>;
  using Fields = typename NamedTupleType::Fields;

  const auto columns = make_columns<Fields>(
      std::make_integer_sequence<int, rfl::tuple_size_v<Fields>>());

  const auto get_name = [](const auto& _col) { return _col.name; };

  auto result = InsertOrWrite{
      .table = dynamic::Table{.alias = std::nullopt,
                              .name = get_tablename<T>(),
                              .schema = get_schema<T>()},
      .columns =
          sqlgen::internal::collect::vector(columns | transform(get_name))};

  if constexpr (std::is_same_v<InsertOrWrite, dynamic::Insert>) {
    const auto is_non_primary = [](const auto& _c) {
      return _c.type.visit(
          [](const auto& _t) { return !_t.properties.primary; });
    };

    const auto is_constraint = [](const auto& _c) {
      return _c.type.visit([](const auto& _t) {
        return _t.properties.primary || _t.properties.unique;
      });
    };

    result.or_replace = or_replace;
    result.non_primary_keys = sqlgen::internal::collect::vector(
        columns | filter(is_non_primary) | transform(get_name));
    if (or_replace) {
      result.constraints = sqlgen::internal::collect::vector(
          columns | filter(is_constraint) | transform(get_name));
    }
  }

  return result;
}

template <class T, class InsertOrWrite>
  requires std::is_class_v<std::remove_cvref_t<T>> &&
           std::is_aggregate_v<std::remove_cvref_t<T>>
InsertOrWrite to_insert_or_write() {
  return to_insert_or_write<T, InsertOrWrite>(false);
}

}  // namespace sqlgen::transpilation

#endif
