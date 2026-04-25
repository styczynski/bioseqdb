#ifndef SQLGEN_DYNAMIC_GROUPBY_HPP_
#define SQLGEN_DYNAMIC_GROUPBY_HPP_

#include <vector>

#include "Column.hpp"

namespace sqlgen::dynamic {

struct GroupBy {
  std::vector<Column> columns;
};

}  // namespace sqlgen::dynamic

#endif
