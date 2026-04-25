#include "sqlgen/sqlite/Connection.hpp"

#include <ranges>
#include <rfl.hpp>
#include <rfl/Variant.hpp>
#include <sstream>

#include "sqlgen/internal/collect/vector.hpp"
#include "sqlgen/internal/strings/strings.hpp"
#include "sqlgen/sqlite/to_sql.hpp"

namespace sqlgen::sqlite {

Connection::Connection(const std::string& _fname)
    : stmt_(nullptr), conn_(make_conn(_fname)) {}

Connection::~Connection() = default;

Result<Nothing> Connection::actual_insert(
    const std::vector<std::vector<std::optional<std::string>>>& _data,
    sqlite3_stmt* _stmt) const noexcept {
  for (const auto& row : _data) {
    const auto num_cols = static_cast<int>(row.size());

    for (int i = 0; i < num_cols; ++i) {
      if (row[i]) {
        const auto res =
            sqlite3_bind_text(_stmt, i + 1, row[i]->c_str(),
                              static_cast<int>(row[i]->size()), SQLITE_STATIC);
        if (res != SQLITE_OK) {
          return error(sqlite3_errmsg(conn_.get()));
        }
      } else {
        const auto res = sqlite3_bind_null(_stmt, i + 1);
        if (res != SQLITE_OK) {
          return error(sqlite3_errmsg(conn_.get()));
        }
      }
    }

    auto res = sqlite3_step(_stmt);
    if (res != SQLITE_OK && res != SQLITE_ROW && res != SQLITE_DONE) {
      return error(sqlite3_errmsg(conn_.get()));
    }

    res = sqlite3_reset(_stmt);
    if (res != SQLITE_OK) {
      return error(sqlite3_errmsg(conn_.get()));
    }
  }

  const auto res = sqlite3_clear_bindings(_stmt);
  if (res != SQLITE_OK) {
    return error(sqlite3_errmsg(conn_.get()));
  }

  return Nothing{};
}

Result<Nothing> Connection::begin_transaction() noexcept {
  return execute("BEGIN TRANSACTION;");
}

Result<Nothing> Connection::commit() noexcept { return execute("COMMIT;"); }

rfl::Result<Ref<Connection>> Connection::make(
    const std::string& _fname) noexcept {
  try {
    return Ref<Connection>::make(_fname);
  } catch (std::exception& e) {
    return error(e.what());
  }
}

Result<Nothing> Connection::execute(const std::string& _sql) noexcept {
  char* errmsg = nullptr;
  sqlite3_exec(conn_.get(), _sql.c_str(), nullptr, nullptr, &errmsg);
  if (errmsg) {
    const auto err = error("Executing '" + _sql + "' failed: " + errmsg);
    sqlite3_free(errmsg);
    return err;
  }
  return Nothing{};
}

Result<Nothing> Connection::insert_impl(
    const dynamic::Insert& _stmt,
    const std::vector<std::vector<std::optional<std::string>>>&
        _data) noexcept {
  const auto sql = to_sql_impl(_stmt);
  return prepare_statement(sql).and_then(
      [&](auto _p_stmt) { return actual_insert(_data, _p_stmt.get()); });
}

typename Connection::ConnPtr Connection::make_conn(const std::string& _fname) {
  sqlite3* conn = nullptr;
  const auto err = sqlite3_open(_fname.c_str(), &conn);
  if (err) {
    throw std::runtime_error("Can't open database: " +
                             std::string(sqlite3_errmsg(conn)));
  }
  return ConnPtr::make(std::shared_ptr<sqlite3>(conn, &sqlite3_close)).value();
}

Result<Ref<Iterator>> Connection::read_impl(
    const rfl::Variant<dynamic::SelectFrom, dynamic::Union>& _query) {
  const auto sql = _query.visit([](const auto& _q) { return to_sql_impl(_q); });

  sqlite3_stmt* p_stmt = nullptr;

  sqlite3_prepare_v2(conn_.get(), /* Database handle */
                     sql.c_str(), /* SQL statement, UTF-8 encoded */
                     sql.size(),  /* Maximum length of zSql in bytes. */
                     &p_stmt,     /* OUT: Statement handle */
                     nullptr      /* OUT: Pointer to unused portion of zSql */
  );

  if (!p_stmt) {
    return error(sqlite3_errmsg(conn_.get()));
  }

  return Ref<sqlite3_stmt>::make(StmtPtr(p_stmt, &sqlite3_finalize))
      .transform([&](auto _stmt) { return Ref<Iterator>::make(_stmt, conn_); });
}

Result<Connection::StmtPtr> Connection::prepare_statement(
    const std::string& _sql) const noexcept {
  sqlite3_stmt* p_stmt = nullptr;

  sqlite3_prepare_v2(conn_.get(),  /* Database handle */
                     _sql.c_str(), /* SQL statement, UTF-8 encoded */
                     _sql.size(),  /* Maximum length of zSql in bytes. */
                     &p_stmt,      /* OUT: Statement handle */
                     nullptr       /* OUT: Pointer to unused portion of zSql */
  );

  if (!p_stmt) {
    return error("Preparing the following statement failed: " + _sql +
                 " Reason: " + sqlite3_errmsg(conn_.get()));
  }

  return StmtPtr(p_stmt, &sqlite3_finalize);
}

Result<Nothing> Connection::rollback() noexcept { return execute("ROLLBACK;"); }

std::string Connection::to_sql(const dynamic::Statement& _stmt) noexcept {
  return sqlite::to_sql_impl(_stmt);
}

Result<Nothing> Connection::start_write(const dynamic::Write& _stmt) {
  if (stmt_) {
    return error(
        "A write operation has already been launched. You need to call "
        ".end_write() before you can start another.");
  }

  const auto sql = to_sql_impl(_stmt);

  return prepare_statement(sql)
      .transform([&](auto&& _stmt) {
        stmt_ = std::move(_stmt);
        return Nothing{};
      })
      .and_then([&](const auto&) { return begin_transaction(); });
}

Result<Nothing> Connection::write_impl(
    const std::vector<std::vector<std::optional<std::string>>>& _data) {
  if (!stmt_) {
    return error(
        " You need to call .start_write(...) before you can call "
        ".write(...).");
  }

  return actual_insert(_data, stmt_.get())
      .or_else([&](const auto& err) -> Result<Nothing> {
        rollback();
        return error(err.what());
      });
}

Result<Nothing> Connection::end_write() {
  if (!stmt_) {
    return error(
        " You need to call .start_write(...) before you can call "
        ".end_write().");
  }
  stmt_ = nullptr;
  return commit().or_else([&](const auto& err) -> Result<Nothing> {
    rollback();
    return error(err.what());
  });
}

}  // namespace sqlgen::sqlite
