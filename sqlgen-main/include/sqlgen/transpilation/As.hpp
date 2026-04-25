#ifndef SQLGEN_TRANSPILATION_AS_HPP_
#define SQLGEN_TRANSPILATION_AS_HPP_

#include <rfl.hpp>

#include "../Literal.hpp"

namespace sqlgen::transpilation {

template <class _ValueType, rfl::internal::StringLiteral _new_name>
struct As {
  using ValueType = _ValueType;
  using NewNameType = Literal<_new_name>;

  ValueType val;
};

}  // namespace sqlgen::transpilation

#endif
