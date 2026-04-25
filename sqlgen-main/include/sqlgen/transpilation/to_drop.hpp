#ifndef SQLGEN_TRANSPILATION_TO_DROP_HPP_
#define SQLGEN_TRANSPILATION_TO_DROP_HPP_

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../Result.hpp"
#include "../dynamic/Drop.hpp"
#include "../dynamic/Table.hpp"
#include "get_schema.hpp"
#include "get_tablename.hpp"
#include "to_condition.hpp"

namespace sqlgen::transpilation {

template <class T>
  requires std::is_class_v<std::remove_cvref_t<T>> &&
           std::is_aggregate_v<std::remove_cvref_t<T>>
dynamic::Drop to_drop(const dynamic::Drop::What _what, const bool _if_exists,
                      const bool _cascade) {
  return dynamic::Drop{.what = _what,
                       .if_exists = _if_exists,
                       .cascade = _cascade,
                       .table = dynamic::Table{.alias = std::nullopt,
                                               .name = get_tablename<T>(),
                                               .schema = get_schema<T>()}};
}

}  // namespace sqlgen::transpilation

#endif
