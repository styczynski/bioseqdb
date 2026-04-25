#ifndef SQLGEN_IF_NOT_EXISTS_HPP_
#define SQLGEN_IF_NOT_EXISTS_HPP_

namespace sqlgen {

struct IfNotExists {};

template <class OtherType>
auto operator|(const OtherType& _o, const IfNotExists&) {
  auto o = _o;
  o.if_not_exists_ = true;
  return o;
}

inline const auto if_not_exists = IfNotExists{};

}  // namespace sqlgen

#endif
