#ifndef SQLGEN_CASCADE_HPP_
#define SQLGEN_CASCADE_HPP_

namespace sqlgen {

struct Cascade {};

template <class OtherType>
auto operator|(const OtherType& _o, const Cascade&) {
  auto o = _o;
  o.cascade_ = true;
  return o;
}

inline const auto cascade = Cascade{};

}  // namespace sqlgen

#endif
