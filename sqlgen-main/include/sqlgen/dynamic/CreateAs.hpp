#ifndef SQLGEN_DYNAMIC_CREATEAS_HPP_
#define SQLGEN_DYNAMIC_CREATEAS_HPP_

#include "SelectFrom.hpp"
#include "Table.hpp"
#include "TableOrView.hpp"

namespace sqlgen::dynamic {

struct CreateAs {
  using What = TableOrView;

  What what;
  Table table_or_view;
  SelectFrom query;
  bool or_replace;
  bool if_not_exists;
};

}  // namespace sqlgen::dynamic

#endif
