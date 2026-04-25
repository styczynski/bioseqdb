#ifndef SQLGEN_PATTERN_HPP_
#define SQLGEN_PATTERN_HPP_

#include <rfl.hpp>

namespace sqlgen {

template <rfl::internal::StringLiteral _regex,
          rfl::internal::StringLiteral _name>
using Pattern = rfl::Pattern<_regex, _name>;

};  // namespace sqlgen

#endif

