#ifndef SQLGEN_DYNAMIC_UPDATE_HPP_
#define SQLGEN_DYNAMIC_UPDATE_HPP_

#include <optional>
#include <vector>

#include "Column.hpp"
#include "ColumnOrValue.hpp"
#include "Condition.hpp"
#include "Table.hpp"

namespace sqlgen::dynamic {

struct Update {
  struct Set {
    Column col;
    ColumnOrValue to;
  };

  Table table;
  std::vector<Set> sets;
  std::optional<Condition> where;
};

}  // namespace sqlgen::dynamic

#endif
