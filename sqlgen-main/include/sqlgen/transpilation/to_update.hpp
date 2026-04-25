#ifndef SQLGEN_TRANSPILATION_TO_UPDATE_HPP_
#define SQLGEN_TRANSPILATION_TO_UPDATE_HPP_

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Result.hpp"
#include "../dynamic/Table.hpp"
#include "../dynamic/Update.hpp"
#include "get_schema.hpp"
#include "get_tablename.hpp"
#include "to_condition.hpp"
#include "to_sets.hpp"

namespace sqlgen::transpilation {

template <class T, class SetsType, class WhereType>
  requires std::is_class_v<std::remove_cvref_t<T>> &&
           std::is_aggregate_v<std::remove_cvref_t<T>>
dynamic::Update to_update(const SetsType& _sets, const WhereType& _where) {
  return dynamic::Update{.table = dynamic::Table{.alias = std::nullopt,
                                                 .name = get_tablename<T>(),
                                                 .schema = get_schema<T>()},
                         .sets = to_sets<T>(_sets),
                         .where = to_condition<std::remove_cvref_t<T>>(_where)};
}

}  // namespace sqlgen::transpilation

#endif
