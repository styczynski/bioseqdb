#include "sqlgen/mysql/Connection.hpp"

#include <cstring>
#include <ranges>
#include <rfl.hpp>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "sqlgen/internal/collect/vector.hpp"
#include "sqlgen/internal/strings/strings.hpp"
#include "sqlgen/mysql/Iterator.hpp"
#include "sqlgen/mysql/make_error.hpp"

namespace sqlgen::mysql {

Connection::Connection(const Credentials& _credentials)
    : conn_(make_conn(_credentials)) {}

Connection::~Connection() = default;

Result<Nothing> Connection::actual_insert(
    const std::vector<std::vector<std::optional<std::string>>>& _data,
    MYSQL_STMT* _stmt) const noexcept {
  const auto num_params = static_cast<size_t>(mysql_stmt_param_count(_stmt));

  std::vector<MYSQL_BIND> bind(num_params);

  std::vector<long unsigned int> lengths(num_params);
  std::vector<my_bool> is_null(num_params);

  for (const auto& row : _data) {
    memset(bind.data(), 0, sizeof(MYSQL_BIND) * num_params);

    if (row.size() != num_params) {
      return error("Expected " + std::to_string(num_params) + " fields, got " +
                   std::to_string(row.size()) + ".");
    }

    auto buffer = row;

    for (size_t i = 0; i < num_params; ++i) {
      if (buffer[i]) {
        lengths[i] = static_cast<long unsigned int>(row[i]->size());
        is_null[i] = 0;

        bind[i].buffer_type = MYSQL_TYPE_STRING;
        bind[i].buffer = &((*buffer[i])[0]);
        bind[i].buffer_length = lengths[i];
        bind[i].is_null = &(is_null[i]);
        bind[i].length = &(lengths[i]);
      } else {
        lengths[i] = 0;
        is_null[i] = 1;

        bind[i].buffer_type = MYSQL_TYPE_NULL;
        bind[i].buffer = nullptr;
        bind[i].buffer_length = 0;
        bind[i].is_null = &(is_null[i]);
        bind[i].length = 0;
      }
    }

    auto err = mysql_stmt_bind_param(_stmt, bind.data());
    if (err) {
      return make_error(conn_);
    }

    err = mysql_stmt_execute(_stmt);
    if (err) {
      return make_error(conn_);
    }
  }

  return Nothing{};
}

Result<Nothing> Connection::begin_transaction() noexcept {
  return execute("START TRANSACTION;");
}

Result<Nothing> Connection::commit() noexcept { return execute("COMMIT;"); }

Result<Nothing> Connection::execute(const std::string& _sql) noexcept {
  return exec(conn_, _sql);
}

Result<Nothing> Connection::insert_impl(
    const dynamic::Insert& _stmt,
    const std::vector<std::vector<std::optional<std::string>>>&
        _data) noexcept {
  if (_data.size() == 0) {
    return Nothing{};
  }
  return prepare_statement(_stmt).and_then(
      [&](auto&& _stmt_ptr) { return actual_insert(_data, _stmt_ptr.get()); });
}

rfl::Result<Ref<Connection>> Connection::make(
    const Credentials& _credentials) noexcept {
  try {
    return Ref<Connection>::make(_credentials);
  } catch (std::exception& e) {
    return error(e.what());
  }
}

typename Connection::ConnPtr Connection::make_conn(
    const Credentials& _credentials) {
  const auto raw_ptr = mysql_init(nullptr);

  const auto shared_ptr = std::shared_ptr<MYSQL>(raw_ptr, mysql_close);

  const auto res = mysql_real_connect(
      shared_ptr.get(), _credentials.host.c_str(), _credentials.user.c_str(),
      _credentials.password.c_str(), _credentials.dbname.c_str(),
      _credentials.port, _credentials.unix_socket.c_str(),
      CLIENT_MULTI_STATEMENTS);

  if (!res) {
    throw std::runtime_error(
        make_error(ConnPtr::make(shared_ptr).value()).error().what());
  }

  return ConnPtr::make(shared_ptr).value();
}

Result<Connection::StmtPtr> Connection::prepare_statement(
    const std::variant<dynamic::Insert, dynamic::Write>& _stmt) const noexcept {
  const auto sql = std::visit(to_sql_impl, _stmt);
  const auto stmt_ptr = StmtPtr(mysql_stmt_init(conn_.get()), mysql_stmt_close);
  const auto err = mysql_stmt_prepare(stmt_ptr.get(), sql.c_str(),
                                      static_cast<unsigned long>(sql.size()));
  if (err) {
    return make_error(conn_);
  }
  return stmt_ptr;
}

Result<Ref<Iterator>> Connection::read_impl(
    const rfl::Variant<dynamic::SelectFrom, dynamic::Union>& _query) {
  const auto sql =
      _query.visit([](const auto& _q) { return mysql::to_sql_impl(_q); });
  const auto err =
      mysql_real_query(conn_.get(), sql.c_str(), static_cast<int>(sql.size()));
  if (err) {
    return make_error(conn_);
  }
  const auto raw_ptr = mysql_use_result(conn_.get());
  if (!raw_ptr) {
    return make_error(conn_);
  }
  return Ref<MYSQL_RES>::make(
             std::shared_ptr<MYSQL_RES>(raw_ptr, mysql_free_result))
      .transform([&](auto&& _res) { return Ref<Iterator>::make(_res, conn_); });
}

Result<Nothing> Connection::rollback() noexcept { return execute("ROLLBACK;"); }

Result<Nothing> Connection::start_write(const dynamic::Write& _write_stmt) {
  if (stmt_) {
    return error(
        "A write operation has already been launched. You need to call "
        ".end_write() before you can start another.");
  }
  return begin_transaction()
      .and_then([&](auto&&) { return prepare_statement(_write_stmt); })
      .transform([&](auto&& _stmt) {
        stmt_ = _stmt;
        return Nothing{};
      })
      .or_else([&](auto&& _err) {
        rollback();
        return error(_err.what());
      });
}

std::string Connection::to_sql(const dynamic::Statement& _stmt) noexcept {
  return to_sql_impl(_stmt);
}

Result<Nothing> Connection::write_impl(
    const std::vector<std::vector<std::optional<std::string>>>& _data) {
  if (!stmt_) {
    return error(
        " You need to call .start_write(...) before you can call "
        ".write(...).");
  }
  return actual_insert(_data, stmt_.get()).or_else([&](const auto& _err) {
    rollback();
    stmt_ = nullptr;
    return error(_err.what());
  });
}

Result<Nothing> Connection::end_write() {
  stmt_ = nullptr;
  return commit();
}

}  // namespace sqlgen::mysql
