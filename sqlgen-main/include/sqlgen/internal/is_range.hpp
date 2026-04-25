#ifndef SQLGEN_TRANSPILATION_IS_RANGE_HPP_
#define SQLGEN_TRANSPILATION_IS_RANGE_HPP_

#include <type_traits>

#include "../Range.hpp"

namespace sqlgen::internal {

template <class T>
class is_range;

template <class T>
class is_range : public std::false_type {};

template <class IteratorType>
class is_range<Range<IteratorType>> : public std::true_type {};

template <class T>
constexpr bool is_range_v = is_range<T>();

}  // namespace sqlgen::internal

#endif
