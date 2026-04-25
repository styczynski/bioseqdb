#ifndef SQLGEN_TRANSPILATION_TO_OFFSET_HPP_
#define SQLGEN_TRANSPILATION_TO_OFFSET_HPP_

#include <rfl.hpp>
#include <type_traits>
#include <vector>

#include "../Result.hpp"
#include "../dynamic/Offset.hpp"
#include "Offset.hpp"
#include "order_by_t.hpp"

namespace sqlgen::transpilation {

template <class OffsetType>
std::optional<dynamic::Offset> to_offset(const OffsetType& _offset) {
  if constexpr (std::is_same_v<std::remove_cvref_t<OffsetType>, Nothing>) {
    return std::nullopt;

  } else if constexpr (std::is_same_v<std::remove_cvref_t<OffsetType>, Offset>) {
    return _offset;

  } else {
    static_assert(rfl::always_false_v<OffsetType>, "Unsupported type");
  }
}

}  // namespace sqlgen::transpilation

#endif
