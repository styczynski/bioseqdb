#ifndef SQLGEN_TRANSPILATION_JOIN_HPP_
#define SQLGEN_TRANSPILATION_JOIN_HPP_

#include <rfl.hpp>

#include "../Literal.hpp"
#include "../dynamic/JoinType.hpp"
#include "TableWrapper.hpp"

namespace sqlgen::transpilation {

using JoinType = dynamic::JoinType;

template <class _TableOrQueryType, class ConditionType,
          rfl::internal::StringLiteral _alias, JoinType _how>
struct Join {
  using TableOrQueryType = _TableOrQueryType;
  using Alias = Literal<_alias>;

  static constexpr JoinType how = _how;

  TableOrQueryType table_or_query;
  ConditionType on;
};

}  // namespace sqlgen::transpilation

#endif
