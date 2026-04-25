#ifndef SQLGEN_DYNAMIC_WRITE_HPP_
#define SQLGEN_DYNAMIC_WRITE_HPP_

#include <string>
#include <vector>

#include "Table.hpp"

namespace sqlgen::dynamic {

struct Write {
  Table table;
  std::vector<std::string> columns;
};

}  // namespace sqlgen::dynamic

#endif
