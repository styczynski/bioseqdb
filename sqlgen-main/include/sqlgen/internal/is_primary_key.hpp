#ifndef SQLGEN_INTERNAL_IS_PRIMARY_KEY_HPP_
#define SQLGEN_INTERNAL_IS_PRIMARY_KEY_HPP_

#include <type_traits>

#include "../PrimaryKey.hpp"

namespace sqlgen::internal {

template <class T>
class is_primary_key;

template <class T>
class is_primary_key : public std::false_type {};

template <class T, bool _auto_incr>
class is_primary_key<PrimaryKey<T, _auto_incr>> : public std::true_type {};

template <class T>
constexpr bool is_primary_key_v = is_primary_key<std::remove_cvref_t<T>>();

}  // namespace sqlgen::internal

#endif
