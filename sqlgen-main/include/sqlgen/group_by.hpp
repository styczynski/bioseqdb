#ifndef SQLGEN_GROUP_BY_HPP_
#define SQLGEN_GROUP_BY_HPP_

namespace sqlgen {

template <class... ColTypes>
struct GroupBy {};

template <class... ColTypes>
auto group_by(const ColTypes&...) {
  return GroupBy<ColTypes...>{};
};

}  // namespace sqlgen

#endif
