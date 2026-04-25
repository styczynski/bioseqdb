#ifndef SQLGEN_DYNAMIC_DELETEFROM_HPP_
#define SQLGEN_DYNAMIC_DELETEFROM_HPP_

#include <optional>

#include "Condition.hpp"
#include "Table.hpp"

namespace sqlgen::dynamic {

struct DeleteFrom {
  Table table;
  std::optional<Condition> where = std::nullopt;
};

}  // namespace sqlgen::dynamic

#endif
