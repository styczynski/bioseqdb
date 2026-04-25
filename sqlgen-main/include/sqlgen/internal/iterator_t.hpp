#ifndef SQLGEN_TRANSPILATION_ITERATOR_T_HPP_
#define SQLGEN_TRANSPILATION_ITERATOR_T_HPP_

#include <type_traits>

#include "../Ref.hpp"
#include "../Result.hpp"

namespace sqlgen::internal {

template <class ValueType, class ConnType>
struct IteratorType;

template <class ValueType, class ConnType>
struct IteratorType<ValueType, Ref<ConnType>> {
  using Type =
      typename IteratorType<ValueType, std::remove_cvref_t<ConnType>>::Type;
};

template <class ValueType, class ConnType>
struct IteratorType<ValueType, Result<ConnType>> {
  using Type =
      typename IteratorType<ValueType, std::remove_cvref_t<ConnType>>::Type;
};

// The specific iterator is implemented by each database in (database
// name)/Connection.hpp

template <class T, class ConnType>
using iterator_t = typename IteratorType<std::remove_cvref_t<T>,
                                         std::remove_cvref_t<ConnType>>::Type;

}  // namespace sqlgen::internal

#endif
