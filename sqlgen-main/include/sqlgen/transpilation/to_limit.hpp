#ifndef SQLGEN_TRANSPILATION_TO_LIMIT_HPP_
#define SQLGEN_TRANSPILATION_TO_LIMIT_HPP_

#include <rfl.hpp>
#include <type_traits>
#include <vector>

#include "../Result.hpp"
#include "../dynamic/Limit.hpp"
#include "Limit.hpp"
#include "order_by_t.hpp"

namespace sqlgen::transpilation {

template <class LimitType>
std::optional<dynamic::Limit> to_limit(const LimitType& _limit) {
  if constexpr (std::is_same_v<std::remove_cvref_t<LimitType>, Nothing>) {
    return std::nullopt;

  } else if constexpr (std::is_same_v<std::remove_cvref_t<LimitType>, Limit>) {
    return _limit;

  } else {
    static_assert(rfl::always_false_v<LimitType>, "Unsupported type");
  }
}

}  // namespace sqlgen::transpilation

#endif
