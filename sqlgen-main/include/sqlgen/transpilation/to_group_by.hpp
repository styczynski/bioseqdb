#ifndef SQLGEN_TRANSPILATION_TO_GROUP_BY_HPP_
#define SQLGEN_TRANSPILATION_TO_GROUP_BY_HPP_

#include <type_traits>
#include <vector>

#include "../Result.hpp"
#include "../dynamic/Column.hpp"
#include "../dynamic/GroupBy.hpp"
#include "group_by_t.hpp"
#include "to_alias.hpp"

namespace sqlgen::transpilation {

template <class T>
struct ToGroupBy;

template <>
struct ToGroupBy<Nothing> {
  std::optional<dynamic::GroupBy> operator()() const { return std::nullopt; }
};

template <class... ColTypes>
struct ToGroupBy<GroupBy<ColTypes...>> {
  std::optional<dynamic::GroupBy> operator()() const {
    const auto columns = std::vector<dynamic::Column>(
        {dynamic::Column{.alias = to_alias<typename ColTypes::Alias>(),
                         .name = ColTypes().name()}...});
    return dynamic::GroupBy{.columns = columns};
  }
};

template <class T>
const auto to_group_by = ToGroupBy<std::remove_cvref_t<T>>{};

}  // namespace sqlgen::transpilation

#endif
