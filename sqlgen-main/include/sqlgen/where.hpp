#ifndef SQLGEN_WHERE_HPP_
#define SQLGEN_WHERE_HPP_

#include <type_traits>

namespace sqlgen {

template <class ConditionType>
struct Where {
  ConditionType condition;
};

template <class ConditionType>
inline auto where(const ConditionType& _cond) {
  return Where<std::remove_cvref_t<ConditionType>>{.condition = _cond};
};

}  // namespace sqlgen

#endif
