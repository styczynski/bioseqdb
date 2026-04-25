#ifndef SQLGEN_DYNAMIC_TABLE_HPP_
#define SQLGEN_DYNAMIC_TABLE_HPP_

#include <optional>
#include <string>

#include "Column.hpp"

namespace sqlgen::dynamic {

struct Table {
  std::optional<std::string> alias = std::nullopt;
  std::string name;
  std::optional<std::string> schema = std::nullopt;
};

}  // namespace sqlgen::dynamic

#endif
