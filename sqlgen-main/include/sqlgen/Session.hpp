#ifndef SQLGEN_SESSION_HPP_
#define SQLGEN_SESSION_HPP_

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "Ref.hpp"
#include "Result.hpp"
#include "dynamic/Insert.hpp"
#include "dynamic/SelectFrom.hpp"
#include "dynamic/Statement.hpp"
#include "dynamic/Write.hpp"
#include "internal/iterator_t.hpp"

namespace sqlgen {

template <class _Connection>
class Session {
 public:
  using Connection = _Connection;
  using ConnPtr = Ref<Connection>;

  Session(const Ref<Connection>& _conn, const Ref<std::atomic_flag>& _flag)
      : conn_(_conn), flag_(_flag.ptr()) {}

  Session(const Session<Connection>& _other) = delete;

  Session(Session<Connection>&& _other)
      : conn_(std::move(_other.conn_)), flag_(_other.flag_) {
    _other.flag_.reset();
  }

  ~Session() {
    if (flag_) {
      flag_->clear();
    }
  }

  Result<Nothing> begin_transaction() { return conn_->begin_transaction(); }

  Result<Nothing> commit() { return conn_->commit(); }

  Result<Nothing> execute(const std::string& _sql) {
    return conn_->execute(_sql);
  }

  template <class ItBegin, class ItEnd>
  Result<Nothing> insert(const dynamic::Insert& _stmt, ItBegin _begin,
                         ItEnd _end) {
    return conn_->insert(_stmt, _begin, _end);
  }

  Session& operator=(const Session& _other) = delete;

  Session& operator=(Session&& _other) noexcept {
    if (this == &_other) {
      return *this;
    }
    conn_ = std::move(_other.conn_);
    flag_ = _other.flag_;
    _other.flag_.reset();
    return *this;
  }

  template <class ContainerType>
  auto read(const dynamic::SelectFrom& _query) {
    return conn_->template read<ContainerType>(_query);
  }

  Result<Nothing> rollback() noexcept { return conn_->rollback(); }

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
  /// The underlying connection object.
  ConnPtr conn_;

  /// The flag corresponding to the object - as long as this is true, we have
  /// ownership.
  std::shared_ptr<std::atomic_flag> flag_;
};

}  // namespace sqlgen

namespace sqlgen::internal {

template <class ValueType, class Connection>
struct IteratorType<ValueType, sqlgen::Session<Connection>> {
  using Type = typename IteratorType<ValueType, Connection>::Type;
};

}  // namespace sqlgen::internal

#endif
