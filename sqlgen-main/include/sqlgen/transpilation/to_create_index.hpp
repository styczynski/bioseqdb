#ifndef SQLGEN_TRANSPILATION_TO_CREATE_INDEX_HPP_
#define SQLGEN_TRANSPILATION_TO_CREATE_INDEX_HPP_

#include <optional>
#include <rfl.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../dynamic/CreateIndex.hpp"
#include "../dynamic/Table.hpp"
#include "get_schema.hpp"
#include "get_tablename.hpp"
#include "to_condition.hpp"

namespace sqlgen::transpilation {

template <class T, class ColumnsType, class WhereType>
  requires std::is_class_v<std::remove_cvref_t<T>> &&
           std::is_aggregate_v<std::remove_cvref_t<T>>
dynamic::CreateIndex to_create_index(const std::string& _name,
                                     const bool _unique,
                                     const bool _if_not_exists,
                                     const WhereType& _where) {
  return dynamic::CreateIndex{
      .name = _name,
      .table = dynamic::Table{.alias = std::nullopt,
                              .name = get_tablename<T>(),
                              .schema = get_schema<T>()},
      .columns = ColumnsType::to_vec(),
      .unique = _unique,
      .if_not_exists = _if_not_exists,
      .where = to_condition<std::remove_cvref_t<T>>(_where)};
}

}  // namespace sqlgen::transpilation

#endif
