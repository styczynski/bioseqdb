#ifndef SQLGEN_CREATEINDEX_HPP_
#define SQLGEN_CREATEINDEX_HPP_

#include <rfl.hpp>

#include "Ref.hpp"
#include "Result.hpp"
#include "is_connection.hpp"
#include "transpilation/columns_t.hpp"
#include "transpilation/to_create_index.hpp"
#include "where.hpp"

namespace sqlgen {

template <class ValueType, class ColumnsType, class WhereType, class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> create_index_impl(const Ref<Connection>& _conn,
                                          const std::string& _name,
                                          const bool _unique,
                                          const bool _if_not_exists,
                                          const WhereType& _where) {
  const auto query =
      transpilation::to_create_index<ValueType, ColumnsType, WhereType>(
          _name, _unique, _if_not_exists, _where);
  return _conn->execute(_conn->to_sql(query)).transform([&](const auto&) {
    return _conn;
  });
}

template <class ValueType, class ColumnsType, class WhereType, class Connection>
  requires is_connection<Connection>
Result<Ref<Connection>> create_index_impl(const Result<Ref<Connection>>& _res,
                                          const std::string& _name,
                                          const bool _unique,
                                          const bool _if_not_exists,
                                          const WhereType& _where) {
  return _res.and_then([&](const auto& _conn) {
    return create_index_impl<ValueType, ColumnsType, WhereType>(
        _conn, _name, _unique, _if_not_exists, _where);
  });
}

template <rfl::internal::StringLiteral _name, class ValueType, class WhereType,
          class... ColTypes>
struct CreateIndex {
  auto operator()(const auto& _conn) const {
    return create_index_impl<
        ValueType,
        transpilation::columns_t<ValueType, typename ColTypes::ColType...>,
        WhereType>(_conn, _name.str(), unique_, if_not_exists_, where_);
  }

  template <class ConditionType>
  friend auto operator|(
      const CreateIndex<_name, ValueType, WhereType, ColTypes...>& _c,
      const Where<ConditionType>& _where) {
    static_assert(std::is_same_v<WhereType, Nothing>,
                  "You cannot call where(...) twice (but you can apply more "
                  "than one condition by combining them with && or ||).");
    return CreateIndex<_name, ValueType, ConditionType, ColTypes...>{
        .unique_ = _c.unique_,
        .if_not_exists_ = _c.if_not_exists_,
        .where_ = _where.condition};
  }

  bool unique_ = false;
  bool if_not_exists_ = false;

  WhereType where_;
};

template <rfl::internal::StringLiteral _name, class ValueType,
          class... ColTypes>
auto create_index(const ColTypes&...) {
  return CreateIndex<_name, ValueType, Nothing, ColTypes...>{
      .unique_ = false, .if_not_exists_ = false};
}

template <rfl::internal::StringLiteral _name, class ValueType,
          class... ColTypes>
auto create_unique_index(const ColTypes&...) {
  return CreateIndex<_name, ValueType, Nothing, ColTypes...>{
      .unique_ = true, .if_not_exists_ = false};
}

};  // namespace sqlgen

#endif

