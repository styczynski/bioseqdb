#ifndef SQLGEN_TRANSPILATION_EXTRACTTABLET_HPP_
#define SQLGEN_TRANSPILATION_EXTRACTTABLET_HPP_

#include <type_traits>

#include "TableWrapper.hpp"
#include "wrap_in_optional_t.hpp"

namespace sqlgen::transpilation {

template <class T, bool _wrap_in_optional>
struct ExtractTable;

template <class T>
struct ExtractTable<T, false> {
  using Type = T;
};

template <class T>
struct ExtractTable<T, true> {
  using Type = wrap_in_optional_t<T>;
};

template <class T>
struct ExtractTable<TableWrapper<T>, false> {
  using Type = typename ExtractTable<std::remove_cvref_t<T>, false>::Type;
};

template <class T>
struct ExtractTable<TableWrapper<T>, true> {
  using Type = typename ExtractTable<std::remove_cvref_t<T>, true>::Type;
};

template <class T, bool _wrap_in_optional>
using extract_table_t =
    typename ExtractTable<std::remove_cvref_t<T>, _wrap_in_optional>::Type;

}  // namespace sqlgen::transpilation

#endif
