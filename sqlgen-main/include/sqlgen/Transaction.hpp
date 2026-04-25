#ifndef SQLGEN_TRANSACTION_HPP_
#define SQLGEN_TRANSACTION_HPP_

#include "Ref.hpp"
#include "internal/iterator_t.hpp"
#include "is_connection.hpp"

namespace sqlgen {

template <class _ConnType>
  requires is_connection<_ConnType>
class Transaction {
 public:
  using ConnType = _ConnType;

  Transaction(const Ref<ConnType>& _conn)
      : conn_(_conn), transaction_ended_(false) {}

  Transaction(const Transaction& _other) = delete;

  Transaction(Transaction&& _other) noexcept
      : conn_(std::move(_other.conn_)),
        transaction_ended_(_other.transaction_ended_) {
    _other.transaction_ended_ = true;
  }

  ~Transaction() {
    if (!transaction_ended_) {
      rollback();
    }
  }

  Result<Nothing> begin_transaction() {
    if (!transaction_ended_) {
      return error("Transaction has already begun, cannot begin another.");
    }
    return conn_->begin_transaction().transform([&](const auto& _nothing) {
      transaction_ended_ = false;
      return _nothing;
    });
  }

  Result<Nothing> commit() {
    if (transaction_ended_) {
      return error("Transaction has already ended, cannot commit.");
    }
    return conn_->commit().transform([&](const auto& _nothing) {
      transaction_ended_ = true;
      return _nothing;
    });
  }

  const Ref<ConnType>& conn() const noexcept { return conn_; }

  Result<Nothing> execute(const std::string& _sql) {
    return conn_->execute(_sql);
  }

  template <class ItBegin, class ItEnd>
  Result<Nothing> insert(const dynamic::Insert& _stmt, ItBegin _begin,
                         ItEnd _end) {
    return conn_->insert(_stmt, _begin, _end);
  }

  Transaction& operator=(const Transaction& _other) = delete;

  Transaction& operator=(Transaction&& _other) noexcept {
    if (this == &_other) {
      return *this;
    }
    conn_ = _other.conn_;
    transaction_ended_ = _other.transaction_ended_;
    _other.transaction_ended_ = true;
    return *this;
  }

  template <class ContainerType>
  auto read(const dynamic::SelectFrom& _query) {
    return conn_->template read<ContainerType>(_query);
  }

  Result<Nothing> rollback() noexcept {
    if (transaction_ended_) {
      return error("Transaction has already ended, cannot roll back.");
    }
    return conn_->rollback().transform([&](const auto& _nothing) {
      transaction_ended_ = true;
      return _nothing;
    });
  }

  std::string to_sql(const dynamic::Statement& _stmt) noexcept {
    return conn_->to_sql(_stmt);
  }

  Result<Nothing> start_write(const dynamic::Write& _stmt) {
    return conn_->start_write(_stmt);
  }

  Result<Nothing> end_write() { return conn_->end_write(); }

  template <class ItBegin, class ItEnd>
  Result<Nothing> write(ItBegin _begin, ItEnd _end) {
    return conn_->write(_begin, _end);
  }

 private:
  Ref<ConnType> conn_;

  bool transaction_ended_;
};

}  // namespace sqlgen

namespace sqlgen::internal {

template <class ValueType, class Connection>
struct IteratorType<ValueType, sqlgen::Transaction<Connection>> {
  using Type = typename IteratorType<ValueType, Connection>::Type;
};

}  // namespace sqlgen::internal
#endif
