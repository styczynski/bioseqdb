#ifndef SQLGEN_DYNAMIC_DROP_HPP_
#define SQLGEN_DYNAMIC_DROP_HPP_

#include <optional>

#include "Table.hpp"
#include "TableOrView.hpp"

namespace sqlgen::dynamic {

struct Drop {
  using What = TableOrView;

  What what;
  bool if_exists = false;
  bool cascade = false;
  Table table;
};

}  // namespace sqlgen::dynamic

#endif
