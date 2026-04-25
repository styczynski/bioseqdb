#ifndef SQLGEN_TRANSPILATION_COLUMNST_HPP_
#define SQLGEN_TRANSPILATION_COLUMNST_HPP_

#include <rfl.hpp>
#include <type_traits>
#include <vector>

#include "Desc.hpp"
#include "all_columns_exist.hpp"

namespace sqlgen::transpilation {

template <class... _ColTypes>
struct Columns {
  rfl::Tuple<_ColTypes...> values;

  static std::vector<std::string> to_vec() {
    return std::vector<std::string>({_ColTypes().name()...});
  }
};

template <class T, class... ColTypes>
auto make_columns() {
  static_assert(all_columns_exist<T, ColTypes...>(),
                "At least one column referenced in your query does not exist.");
  return Columns<ColTypes...>{};
}

template <class T, class... ColTypes>
using columns_t = std::invoke_result_t<decltype(make_columns<T, ColTypes...>)>;

}  // namespace sqlgen::transpilation

#endif
