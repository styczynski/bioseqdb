#ifndef SQLGEN_INTERNAL_IS_UNIQUE_COLUMN_HPP_
#define SQLGEN_INTERNAL_IS_UNIQUE_COLUMN_HPP_

#include <type_traits>

#include "../Unique.hpp"

namespace sqlgen::internal {

template <class T>
class is_unique_column;

template <class T>
class is_unique_column : public std::false_type {};

template <class T>
class is_unique_column<Unique<T>> : public std::true_type {};

template <class T>
constexpr bool is_unique_column_v = is_unique_column<std::remove_cvref_t<T>>();

}  // namespace sqlgen::internal

#endif
