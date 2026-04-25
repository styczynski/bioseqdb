#ifndef SQLGEN_INTERNAL_TO_STR_VEC_HPP_
#define SQLGEN_INTERNAL_TO_STR_VEC_HPP_

#include <optional>
#include <rfl.hpp>
#include <string>
#include <type_traits>
#include <vector>

#include "remove_auto_incr_primary_t.hpp"
#include "to_str.hpp"

namespace sqlgen::internal {

template <class T>
std::vector<std::optional<std::string>> to_str_vec(const T& _t) {
  const auto view = rfl::to_view(_t);
  using ViewType = remove_auto_incr_primary_t<decltype(view)>;
  return rfl::apply(
      [](auto... _ptrs) {
        return std::vector<std::optional<std::string>>({to_str(*_ptrs)...});
      },
      ViewType(view).values());
}

}  // namespace sqlgen::internal

#endif
