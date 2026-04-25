#ifndef SQLGEN_DYNAMIC_INSERT_HPP_
#define SQLGEN_DYNAMIC_INSERT_HPP_

#include <string>
#include <vector>

#include "Table.hpp"

namespace sqlgen::dynamic {

struct Insert {
  Table table;
  std::vector<std::string> columns;
  bool or_replace;
  std::vector<std::string> non_primary_keys;

  /// Holds primary keys and unique columns when or_replace is true.
  std::vector<std::string> constraints;
};

}  // namespace sqlgen::dynamic

#endif
