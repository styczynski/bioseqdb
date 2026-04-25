#ifndef SQLGEN_CREATETABLE_HPP_
#define SQLGEN_CREATETABLE_HPP_

#include <rfl.hpp>

#include "is_connection.hpp"
#include "transpilation/to_create_table.hpp"

namespace sqlgen {

template <class ValueType, class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> create_table_impl(const Ref<Connection>& _conn,
                                          const bool _if_not_exists) {
  const auto query = transpilation::to_create_table<ValueType>(_if_not_exists);
  return _conn->execute(_conn->to_sql(query)).transform([&](const auto&) {
    return _conn;
  });
}

template <class ValueType, class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> create_table_impl(const Result<Ref<Connection>>& _res,
                                          const bool _if_not_exists) {
  return _res.and_then([&](const auto& _conn) {
    return create_table_impl<ValueType>(_conn, _if_not_exists);
  });
}

template <class ValueType>
struct CreateTable {
  auto operator()(const auto& _conn) const {
    return create_table_impl<ValueType>(_conn, if_not_exists_);
  }

  bool if_not_exists_;
};

template <class ValueType>
const auto create_table = CreateTable<ValueType>{};

};  // namespace sqlgen

#endif

