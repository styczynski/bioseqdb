#ifndef SQLGEN_POSTGRES_CONNECTION_HPP_
#define SQLGEN_POSTGRES_CONNECTION_HPP_

#include <libpq-fe.h>

#include <memory>
#include <optional>
#include <rfl.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <list>

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
#include "../internal/iterator_t.hpp"
#include "../internal/to_container.hpp"
#include "../internal/write_or_insert.hpp"
#include "../is_connection.hpp"
#include "../sqlgen_api.hpp"
#include "../transpilation/value_t.hpp"
#include "Credentials.hpp"
#include "Iterator.hpp"
#include "PostgresV2Connection.hpp"
#include "exec.hpp"
#include "to_sql.hpp"

namespace sqlgen::postgres {

enum class NotificationWaitResult {
 Ready, // Data available (possibly a NOTIFY)
 Timeout, // Timeout elapsed
 Error // I/O or connection error
};

struct Notification {
 std::string channel;
 std::string payload;
 int backend_pid;
};

class SQLGEN_API Connection {
  using Conn = PostgresV2Connection;

 public:
  Connection(const Conn& _conn);

  Connection(const Credentials& _credentials);

  static rfl::Result<Ref<Connection>> make(
      const Credentials& _credentials) noexcept;

  ~Connection() = default;

  Result<Nothing> begin_transaction() noexcept;

  Result<Nothing> commit() noexcept;

  Result<Nothing> execute(const std::string& _sql) noexcept;

  template <class... Args>
  Result<Nothing> execute(const std::string& _sql, Args&&... _args) noexcept {
    return execute_params(_sql, {to_param(std::forward<Args>(_args))...});
  }

 private:
  template <class T>
  static std::optional<std::string> to_param(const T& _val) {
    if constexpr (std::is_same_v<std::decay_t<T>, std::nullopt_t>) {
      return std::nullopt;
    } else if constexpr (std::is_same_v<std::decay_t<T>, std::nullptr_t>) {
      return std::nullopt;
    } else if constexpr (std::is_same_v<std::decay_t<T>, std::string>) {
      return _val;
    } else if constexpr (std::is_same_v<std::decay_t<T>, const char*> ||
                         std::is_same_v<std::decay_t<T>, char*>) {
      return _val ? std::optional<std::string>(_val) : std::nullopt;
    } else if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
      return _val ? "true" : "false";
    } else if constexpr (std::is_arithmetic_v<std::decay_t<T>>) {
      return std::to_string(_val);
    } else {
      static_assert(std::is_convertible_v<T, std::string>,
                    "Parameter type must be convertible to string");
      return std::string(_val);
    }
  }

  template <class T>
  static std::optional<std::string> to_param(const std::optional<T>& _val) {
    return _val ? to_param(*_val) : std::nullopt;
  }

  Result<Nothing> execute_params(
      const std::string& _sql,
      const std::vector<std::optional<std::string>>& _params) noexcept;

 public:

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
          return sqlgen::Iterator<ValueType, postgres::Iterator>(
              std::move(_it));
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

  std::list<Notification> get_notifications() noexcept;

  rfl::Result<Nothing> listen(const std::string& channel) noexcept;

  rfl::Result<Nothing> unlisten(const std:: string& channel) noexcept;

  rfl::Result<Nothing> notify(const std::string& channel, const std::string& payload = "") noexcept;

  bool consume_input() noexcept;

 private:
  Result<Nothing> insert_impl(
      const dynamic::Insert& _stmt,
      const std::vector<std::vector<std::optional<std::string>>>&
          _data) noexcept;

  Result<Ref<Iterator>> read_impl(
      const rfl::Variant<dynamic::SelectFrom, dynamic::Union>& _query);

  std::string to_buffer(
      const std::vector<std::optional<std::string>>& _line) const noexcept;

  Result<Nothing> write_impl(
      const std::vector<std::vector<std::optional<std::string>>>& _data);

  bool is_valid_channel_name(const std::string& s) const noexcept;

 private:
  Conn conn_;
};

}  // namespace sqlgen::postgres

namespace sqlgen::internal {

template <class ValueType>
struct IteratorType<ValueType, postgres::Connection> {
  using Type = Iterator<ValueType, postgres::Iterator>;
};

static_assert(is_connection<postgres::Connection>,
              "Must fulfill the is_connection concept.");
static_assert(is_connection<Session<postgres::Connection>>,
              "Must fulfill the is_connection concept.");
static_assert(is_connection<Transaction<postgres::Connection>>,
              "Must fulfill the is_connection concept.");

}  // namespace sqlgen::internal

#endif
