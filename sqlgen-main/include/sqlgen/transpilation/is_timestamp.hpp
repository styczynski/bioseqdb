#ifndef SQLGEN_TRANSPILATION_IS_TIMESTAMP_HPP_
#define SQLGEN_TRANSPILATION_IS_TIMESTAMP_HPP_

#include <rfl.hpp>
#include <type_traits>

#include "../PrimaryKey.hpp"

namespace sqlgen::transpilation {

template <class T>
class is_timestamp;

template <class T>
class is_timestamp : public std::false_type {};

template <rfl::internal::StringLiteral _format>
class is_timestamp<rfl::Timestamp<_format>> : public std::true_type {};

template <class T>
constexpr bool is_timestamp_v = is_timestamp<std::remove_cvref_t<T>>::value;

}  // namespace sqlgen::transpilation

#endif
