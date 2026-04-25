#ifndef SQLGEN_DELETE_FROM_HPP_
#define SQLGEN_DELETE_FROM_HPP_

#include <type_traits>

#include "Ref.hpp"
#include "Result.hpp"
#include "is_connection.hpp"
#include "transpilation/to_delete_from.hpp"
#include "where.hpp"

namespace sqlgen {

template <class ValueType, class WhereType, class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> delete_from_impl(const Ref<Connection>& _conn,
                                         const WhereType& _where) {
  const auto query =
      transpilation::to_delete_from<ValueType, WhereType>(_where);
  return _conn->execute(_conn->to_sql(query)).transform([&](const auto&) {
    return _conn;
  });
}

template <class ValueType, class WhereType, class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> delete_from_impl(const Result<Ref<Connection>>& _res,
                                         const WhereType& _where) {
  return _res.and_then([&](const auto& _conn) {
    return delete_from_impl<ValueType, WhereType>(_conn, _where);
  });
}

template <class ValueType, class WhereType = Nothing>
struct DeleteFrom {
  auto operator()(const auto& _conn) const {
    return delete_from_impl<ValueType, WhereType>(_conn, where_);
  }

  template <class ConditionType>
  friend auto operator|(const DeleteFrom<ValueType, WhereType>& _d,
                        const Where<ConditionType>& _where) {
    static_assert(std::is_same_v<WhereType, Nothing>,
                  "You cannot call where(...) twice (but you can apply more "
                  "than one condition by combining them with && or ||).");
    return DeleteFrom<ValueType, ConditionType>{.where_ = _where.condition};
  }

  WhereType where_;
};

template <class ContainerType>
const auto delete_from = DeleteFrom<ContainerType>{};

}  // namespace sqlgen

#endif
