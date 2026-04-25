#ifndef SQLGEN_INTERNAL_QUERYVALUET_HPP_
#define SQLGEN_INTERNAL_QUERYVALUET_HPP_

#include <type_traits>

#include "../read.hpp"
#include "../select_from.hpp"

namespace sqlgen::internal {

template <class QueryT, class ConnectionT>
struct QueryValueType;

template <class ConnectionT, class... Args>
struct QueryValueType<Read<Args...>, ConnectionT> {
  using Type = std::invoke_result_t<Read<Args...>, ConnectionT>;
};

template <class ConnectionT, class... Args>
struct QueryValueType<SelectFrom<Args...>, ConnectionT> {
  using Type = std::invoke_result_t<SelectFrom<Args...>, ConnectionT>;
};

template <class QueryT, class ConnectionT>
using query_value_t =
    typename QueryValueType<QueryT, ConnectionT>::Type::value_type;

}  // namespace sqlgen::internal

#endif
