#ifndef SQLGEN_COMMIT_HPP_
#define SQLGEN_COMMIT_HPP_

#include <type_traits>

#include "Ref.hpp"
#include "Result.hpp"
#include "Transaction.hpp"

namespace sqlgen {

template <class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> commit_impl(const Ref<Transaction<Connection>>& _t) {
  return _t->commit().transform([&](const auto&) { return _t->conn(); });
}

template <class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> commit_impl(
    const Result<Transaction<Connection>>& _res) {
  return _res.and_then([&](const auto& _t) { return commit_impl(_t); });
}

struct Commit {
  auto operator()(const auto& _conn) const { return commit_impl(_conn); }
};

inline const auto commit = Commit{};

}  // namespace sqlgen

#endif
