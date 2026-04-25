#ifndef SQLGEN_BEGIN_TRANSACTION_HPP_
#define SQLGEN_BEGIN_TRANSACTION_HPP_

#include <type_traits>

#include "Ref.hpp"
#include "Result.hpp"
#include "Transaction.hpp"
#include "is_connection.hpp"

namespace sqlgen {

template <class Connection>
  requires is_connection<Connection>
Result<Ref<Transaction<Connection>>> begin_transaction_impl(
    const Ref<Connection>& _conn) {
  return _conn->begin_transaction().transform(
      [&](const auto&) { return Ref<Transaction<Connection>>::make(_conn); });
}

template <class Connection>
  requires is_connection<Connection>
Result<Ref<Transaction<Connection>>> begin_transaction_impl(
    const Result<Ref<Connection>>& _res) {
  return _res.and_then(
      [&](const auto& _conn) { return begin_transaction_impl(_conn); });
}

struct BeginTransaction {
  auto operator()(const auto& _conn) const {
    return begin_transaction_impl(_conn);
  }
};

inline const auto begin_transaction = BeginTransaction{};

}  // namespace sqlgen

#endif
