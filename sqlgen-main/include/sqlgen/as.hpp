#ifndef SQLGEN_AS_HPP_
#define SQLGEN_AS_HPP_

#include <rfl.hpp>
#include <string>
#include <type_traits>

#include "col.hpp"
#include "transpilation/As.hpp"

namespace sqlgen {

template <rfl::internal::StringLiteral _new_name>
struct As {
  template <class ValueType>
  auto operator()(const ValueType& _val) const noexcept {
    if constexpr (requires(ValueType v) { v.template as<"new_name">(); }) {
      return _val.template as<_new_name>();
    } else {
      return transpilation::As<std::remove_cvref_t<ValueType>, _new_name>{
          .val = _val};
    }
  }

  auto operator()(const char* _val) const noexcept {
    return transpilation::As<std::string, _new_name>{.val = _val};
  }
};

template <class ValueType, rfl::internal::StringLiteral _new_name>
auto operator|(const ValueType& _val, const As<_new_name>& _as) {
  return _as(_val);
}

template <rfl::internal::StringLiteral _new_name>
const auto as = As<_new_name>{};

}  // namespace sqlgen

#endif
