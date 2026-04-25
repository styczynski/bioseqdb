#ifndef SQLGEN_EXEC_HPP_
#define SQLGEN_EXEC_HPP_

#include <rfl.hpp>

#include "Ref.hpp"
#include "Result.hpp"
#include "is_connection.hpp"

namespace sqlgen {

template <class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> exec(const Ref<Connection>& _conn,
                             const std::string& _sql) {
  return _conn->execute(_sql).transform([&](const auto&) { return _conn; });
}

template <class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> exec(const Result<Ref<Connection>>& _res,
                             const std::string& _sql) {
  return _res.and_then([&](const auto& _conn) { return exec(_conn, _sql); });
}

struct Exec {
  auto operator()(const auto& _conn) const { return exec(_conn, sql_); }

  std::string sql_;
};

inline auto exec(const std::string& _sql) { return Exec{.sql_ = _sql}; }

};  // namespace sqlgen

#endif

