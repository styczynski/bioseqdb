#ifndef SQLGEN_DYNAMIC_SELECTFROM_HPP_
#define SQLGEN_DYNAMIC_SELECTFROM_HPP_

#include <optional>
#include <rfl.hpp>
#include <string>
#include <vector>

#include "../Ref.hpp"
#include "Condition.hpp"
#include "GroupBy.hpp"
#include "JoinType.hpp"
#include "Limit.hpp"
#include "Offset.hpp"
#include "Operation.hpp"
#include "OrderBy.hpp"
#include "Table.hpp"

namespace sqlgen::dynamic {

struct SelectFrom {
  struct Union {
    std::vector<std::string> columns;
    Ref<std::vector<SelectFrom>> selects;
    bool all = false;
  };

  using TableOrQueryType = rfl::Variant<Table, Ref<SelectFrom>, Ref<Union>>;

  struct Field {
    Operation val;
    std::optional<std::string> as;
  };

  struct Join {
    JoinType how;
    TableOrQueryType table_or_query;
    std::string alias;
    std::optional<Condition> on;
  };

  TableOrQueryType table_or_query;
  std::vector<Field> fields;
  std::optional<std::string> alias = std::nullopt;
  std::optional<std::vector<Join>> joins = std::nullopt;
  std::optional<Condition> where = std::nullopt;
  std::optional<GroupBy> group_by = std::nullopt;
  std::optional<OrderBy> order_by = std::nullopt;
  std::optional<Limit> limit = std::nullopt;
  std::optional<Offset> offset = std::nullopt;
};

}  // namespace sqlgen::dynamic

#endif
