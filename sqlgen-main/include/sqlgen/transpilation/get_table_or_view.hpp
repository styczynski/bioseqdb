#ifndef SQLGEN_TRANSPILATION_GET_TABLE_OR_VIEW_HPP_
#define SQLGEN_TRANSPILATION_GET_TABLE_OR_VIEW_HPP_

#include <concepts>
#include <type_traits>

#include "../dynamic/TableOrView.hpp"

namespace sqlgen::transpilation {

template <class Type>
consteval dynamic::TableOrView get_table_or_view() {
  using T = std::remove_cvref_t<Type>;

  constexpr bool has_view = requires {
    { T::is_view } -> std::convertible_to<bool>;
  };

  if constexpr (has_view) {
    if constexpr (T::is_view) {
      return dynamic::TableOrView::view;
    }
  } else {
    return dynamic::TableOrView::table;
  }
}

}  // namespace sqlgen::transpilation

#endif
