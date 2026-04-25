#ifndef SQLGEN_INTERNAL_GETCOLTYPE_HPP_
#define SQLGEN_INTERNAL_GETCOLTYPE_HPP_

#include <rfl.hpp>

#include "../col.hpp"
#include "../transpilation/Col.hpp"

namespace sqlgen::internal {

template <class T>
struct GetColType;

template <class T>
struct GetColType {
  using Type = T;
  static Type get_value(const T& _t) { return _t; }
};

template <rfl::internal::StringLiteral _name,
          rfl::internal::StringLiteral _alias>
struct GetColType<Col<_name, _alias>> {
  using Type = transpilation::Col<_name, _alias>;
  static Type get_value(const auto&) {
    return transpilation::Col<_name, _alias>{};
  }
};

template <class T>
using get_col_type_t = typename GetColType<T>::Type;

}  // namespace sqlgen::internal

#endif
