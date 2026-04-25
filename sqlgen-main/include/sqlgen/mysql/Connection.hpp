#ifndef SQLGEN_MYSQL_CONNECTION_HPP_
#define SQLGEN_MYSQL_CONNECTION_HPP_

#include <mysql.h>

#include <memory>
#include <rfl.hpp>
#include <stdexcept>
#include <string>

#include "../Iterator.hpp"
#include "../Ref.hpp"
#include "../Result.hpp"
#include "../Session.hpp"
#include "../Transaction.hpp"
#include "../dynamic/Column.hpp"
#include "../dynamic/Insert.hpp"
#include "../dynamic/SelectFrom.hpp"
#include "../dynamic/Statement.hpp"
#include "../dynamic/Union.hpp"
#include "../dynamic/Write.hpp"
#include "../internal/to_container.hpp"
#include "../internal/write_or_insert.hpp"
#include "../is_connection.hpp"
#include "../sqlgen_api.hpp"
#include "../transpilation/value_t.hpp"
#include "Credentials.hpp"
#include "Iterator.hpp"
#include "exec.hpp"
#include "to_sql.hpp"

namespace sqlgen::mysql {

class SQLGEN_API Connection {
  using ConnPtr = Ref<MYSQL>;
  using StmtPtr = std::shared_ptr<MYSQL_STMT>;

 public:
  Connection(const Credentials& _credentials);

  static rfl::Result<Ref<Connection>> make(
      const Credentials& _credentials) noexcept;

  ~Connection();

  Result<Nothing> begin_transaction() noexcept;

  Result<Nothing> commit() noexcept;

  Result<Nothing> execute(const std::string& _sql) noexcept;

  template <class ItBegin, class ItEnd>
  Result<Nothing> insert(const dynamic::Insert& _stmt, ItBegin _begin,
                         ItEnd _end) noexcept {
    return internal::write_or_insert(
        [&](const auto& _data) { return insert_impl(_stmt, _data); }, _begin,
        _end);
  }

  template <class ContainerType>
  auto read(const rfl::Variant<dynamic::SelectFrom, dynamic::Union>& _query) {
    using ValueType = transpilation::value_t<ContainerType>;
    return internal::to_container<ContainerType>(
        read_impl(_query).transform([](auto&& _it) {
          return sqlgen::Iterator<ValueType, mysql::Iterator>(std::move(_it));
        }));
  }

  Result<Nothing> rollback() noexcept;

  std::string to_sql(const dynamic::Statement& _stmt) noexcept;

  Result<Nothing> start_write(const dynamic::Write& _stmt);

  template <class ItBegin, class ItEnd>
  Result<Nothing> write(ItBegin _begin, ItEnd _end) {
    return internal::write_or_insert(
        [&](const auto& _data) { return write_impl(_data); }, _begin, _end);
  }

  Result<Nothing> end_write();

 private:
  /// Actually inserts data based on a prepared statement -
  /// used by both .insert(...) and .write(...).
  Result<Nothing> actual_insert(
      const std::vector<std::vector<std::optional<std::string>>>& _data,
      MYSQL_STMT* _stmt) const noexcept;

  Result<Nothing> insert_impl(
      const dynamic::Insert& _stmt,
      const std::vector<std::vector<std::optional<std::string>>>&
          _data) noexcept;

  static ConnPtr make_conn(const Credentials& _credentials);

  Result<StmtPtr> prepare_statement(
      const std::variant<dynamic::Insert, dynamic::Write>& _stmt)
      const noexcept;

  Result<Ref<Iterator>> read_impl(
      const rfl::Variant<dynamic::SelectFrom, dynamic::Union>& _query);

  Result<Nothing> write_impl(
      const std::vector<std::vector<std::optional<std::string>>>& _data);

 private:
  /// A prepared statement - needed for the read and write operations. Note that
  /// we have declared it before conn_, meaning it will be destroyed first.
  StmtPtr stmt_;

  /// The underlying connection.
  ConnPtr conn_;
};

}  // namespace sqlgen::mysql

namespace sqlgen::internal {

template <class ValueType>
struct IteratorType<ValueType, mysql::Connection> {
  using Type = Iterator<ValueType, mysql::Iterator>;
};

static_assert(is_connection<mysql::Connection>,
              "Must fulfill the is_connection concept.");
static_assert(is_connection<Session<mysql::Connection>>,
              "Must fulfill the is_connection concept.");
static_assert(is_connection<Transaction<mysql::Connection>>,
              "Must fulfill the is_connection concept.");

}  // namespace sqlgen::internal

#endif
