#ifndef SQLGEN_SQLITE_CONNECTION_HPP_
#define SQLGEN_SQLITE_CONNECTION_HPP_

#include <sqlite3.h>

#include <memory>
#include <rfl.hpp>
#include <sstream>
#include <stdexcept>
#include <string>

#include "../Iterator.hpp"
#include "../Ref.hpp"
#include "../Result.hpp"
#include "../Session.hpp"
#include "../Transaction.hpp"
#include "../dynamic/SelectFrom.hpp"
#include "../dynamic/Union.hpp"
#include "../dynamic/Write.hpp"
#include "../internal/to_container.hpp"
#include "../internal/write_or_insert.hpp"
#include "../is_connection.hpp"
#include "../sqlgen_api.hpp"
#include "../transpilation/value_t.hpp"
#include "Iterator.hpp"
#include "to_sql.hpp"

namespace sqlgen::sqlite {

class SQLGEN_API Connection {
  using ConnPtr = Ref<sqlite3>;
  using StmtPtr = std::shared_ptr<sqlite3_stmt>;

 public:
  Connection(const std::string& _fname);

  static rfl::Result<Ref<Connection>> make(const std::string& _fname) noexcept;

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
          return sqlgen::Iterator<ValueType, sqlite::Iterator>(std::move(_it));
        }));
  }

  Result<Nothing> rollback() noexcept;

  std::string to_sql(const dynamic::Statement& _stmt) noexcept;

  Result<Nothing> start_write(const dynamic::Write& _stmt);

  Result<Nothing> end_write();

  template <class ItBegin, class ItEnd>
  Result<Nothing> write(ItBegin _begin, ItEnd _end) {
    return internal::write_or_insert(
        [&](const auto& _data) { return write_impl(_data); }, _begin, _end);
  }

 private:
  /// Generates the underlying connection.
  static ConnPtr make_conn(const std::string& _fname);

  /// Actually inserts data based on a prepared statement -
  /// used by both .insert(...) and .write(...).
  Result<Nothing> actual_insert(
      const std::vector<std::vector<std::optional<std::string>>>& _data,
      sqlite3_stmt* _stmt) const noexcept;

  /// Implements the actual insert.
  Result<Nothing> insert_impl(
      const dynamic::Insert& _stmt,
      const std::vector<std::vector<std::optional<std::string>>>&
          _data) noexcept;

  /// Generates a prepared statment, usually for inserts.
  Result<StmtPtr> prepare_statement(const std::string& _sql) const noexcept;

  /// Implements the actual read.
  Result<Ref<Iterator>> read_impl(
      const rfl::Variant<dynamic::SelectFrom, dynamic::Union>& _query);

  /// Implements the actual write
  Result<Nothing> write_impl(
      const std::vector<std::vector<std::optional<std::string>>>& _data);

 private:
  /// A prepared statement - needed for the read and write operations. Note that
  /// we have declared it before conn_, meaning it will be destroyed first.
  StmtPtr stmt_;

  /// The underlying sqlite3 connection.
  ConnPtr conn_;
};

}  // namespace sqlgen::sqlite

namespace sqlgen::internal {

template <class ValueType>
struct IteratorType<ValueType, sqlite::Connection> {
  using Type = Iterator<ValueType, sqlite::Iterator>;
};

static_assert(is_connection<sqlite::Connection>,
              "Must fulfill the is_connection concept.");
static_assert(is_connection<Session<sqlite::Connection>>,
              "Must fulfill the is_connection concept.");
static_assert(is_connection<Transaction<sqlite::Connection>>,
              "Must fulfill the is_connection concept.");
}  // namespace sqlgen::internal

#endif
