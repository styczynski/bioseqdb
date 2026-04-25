#ifndef SQLGEN_IF_EXISTS_HPP_
#define SQLGEN_IF_EXISTS_HPP_

#include "drop.hpp"

namespace sqlgen {

struct IfExists {};

template <class OtherType>
auto operator|(const OtherType& _o, const IfExists&) {
  auto o = _o;
  o.if_exists_ = true;
  return o;
}

inline const auto if_exists = IfExists{};

}  // namespace sqlgen

#endif
