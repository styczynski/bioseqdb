#ifndef SQLGEN_TRANSPILATION_COL_HPP_
#define SQLGEN_TRANSPILATION_COL_HPP_

#include <rfl.hpp>
#include <string>

namespace sqlgen::transpilation {

template <rfl::internal::StringLiteral _name,
          rfl::internal::StringLiteral _alias = "">
struct Col {
  using ColType = Col<_name, _alias>;
  using Name = rfl::Literal<_name>;
  using Alias = rfl::Literal<_alias>;

  /// Returns the column alias.
  std::string alias() const noexcept { return Alias().str(); }

  /// Returns the column name.
  std::string name() const noexcept { return Name().str(); }
};

}  // namespace sqlgen::transpilation

#endif
