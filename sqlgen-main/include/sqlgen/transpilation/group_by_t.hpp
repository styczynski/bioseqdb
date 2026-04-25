#ifndef SQLGEN_TRANSPILATION_GROUPBYT_HPP_
#define SQLGEN_TRANSPILATION_GROUPBYT_HPP_

#include <rfl.hpp>
#include <type_traits>
#include <vector>

#include "Col.hpp"
#include "all_columns_exist.hpp"

namespace sqlgen::transpilation {

template <class... ColTypes>
struct GroupBy {};

template <class T, class... ColTypes>
auto make_group_by() {
  static_assert(all_columns_exist<T, ColTypes...>(),
                "A column in group_by does not exist.");
  return GroupBy<ColTypes...>{};
}

template <class T, class... ColTypes>
using group_by_t =
    std::invoke_result_t<decltype(make_group_by<T, ColTypes...>)>;

}  // namespace sqlgen::transpilation

#endif
