#ifndef SQLGEN_INTERNAL_WRITE_OR_INSERT_HPP_
#define SQLGEN_INTERNAL_WRITE_OR_INSERT_HPP_

#include <optional>
#include <string>
#include <vector>

#include "batch_size.hpp"
#include "to_str_vec.hpp"

namespace sqlgen::internal {

template <class FuncType, class ItBegin, class ItEnd>
Result<Nothing> write_or_insert(const FuncType& _actual_insert, ItBegin _begin,
                                ItEnd _end) noexcept {
  std::vector<std::vector<std::optional<std::string>>> data;
  for (auto it = _begin; it != _end; ++it) {
    data.emplace_back(to_str_vec(*it));
    if (data.size() == SQLGEN_BATCH_SIZE) {
      const auto res = _actual_insert(data);
      if (!res) {
        return res;
      }
      data.clear();
    }
  }
  if (data.size() != 0) {
    return _actual_insert(data);
  }
  return Nothing{};
}

}  // namespace sqlgen::internal

#endif
