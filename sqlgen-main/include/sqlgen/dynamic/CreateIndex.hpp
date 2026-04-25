#ifndef SQLGEN_DYNAMIC_CREATEINDEX_HPP_
#define SQLGEN_DYNAMIC_CREATEINDEX_HPP_

#include <string>
#include <vector>

#include "Column.hpp"
#include "Condition.hpp"
#include "Table.hpp"

namespace sqlgen::dynamic {

struct CreateIndex {
  std::string name;

  Table table;
  std::vector<std::string> columns;

  bool unique = false;
  bool if_not_exists = true;
  std::optional<Condition> where = std::nullopt;
};

}  // namespace sqlgen::dynamic

#endif
