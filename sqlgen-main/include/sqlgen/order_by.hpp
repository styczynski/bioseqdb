#ifndef SQLGEN_ORDER_BY_HPP_
#define SQLGEN_ORDER_BY_HPP_

namespace sqlgen {

template <class... ColTypes>
struct OrderBy {};

template <class... ColTypes>
auto order_by(const ColTypes&...) {
  return OrderBy<ColTypes...>{};
};

}  // namespace sqlgen

#endif
