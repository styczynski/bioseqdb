#ifndef SQLGEN_DYNAMIC_CREATETABLE_HPP_
#define SQLGEN_DYNAMIC_CREATETABLE_HPP_

#include <string>
#include <vector>

#include "Column.hpp"
#include "Table.hpp"

namespace sqlgen::dynamic {

struct CreateTable {
  Table table;
  std::vector<Column> columns;
  bool if_not_exists = true;
};

}  // namespace sqlgen::dynamic

#endif
