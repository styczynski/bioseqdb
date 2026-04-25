#ifndef SQLGEN_LITERAL_HPP_
#define SQLGEN_LITERAL_HPP_

#include <rfl.hpp>

namespace sqlgen {

template <rfl::internal::StringLiteral... _names>
using Literal = rfl::Literal<_names...>;

};  // namespace sqlgen

#endif

