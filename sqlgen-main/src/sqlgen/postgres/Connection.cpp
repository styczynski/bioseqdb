#include "sqlgen/postgres/Connection.hpp"

#include <ranges>
#include <rfl.hpp>
#include <sstream>
#include <stdexcept>

#include "sqlgen/internal/collect/vector.hpp"
#include "sqlgen/internal/random.hpp"
#include "sqlgen/internal/strings/strings.hpp"
#include "sqlgen/postgres/Iterator.hpp"
#include "sqlgen/postgres/PostgresV2Result.hpp"

namespace sqlgen::postgres {

Connection::Connection(const Conn& _conn) : conn_(_conn) {}

Connection::Connection(const Credentials& _credentials)
    : conn_(PostgresV2Connection::make(_credentials.to_str(),
                                       _credentials.notice_handler)
                .value()) {}

Result<Nothing> Connection::begin_transaction() noexcept {
  return execute("BEGIN TRANSACTION;");
}

Result<Nothing> Connection::commit() noexcept { return execute("COMMIT;"); }

Result<Nothing> Connection::execute(const std::string& _sql) noexcept {
  return PostgresV2Result::make(_sql, conn_).transform([](auto&&) {
    return Nothing{};
  });
}

Result<Nothing> Connection::execute_params(
    const std::string& _sql,
    const std::vector<std::optional<std::string>>& _params) noexcept {
  return PostgresV2Result::make(_sql, conn_, _params).transform([](auto&&) {
    return Nothing{};
  });
}

Result<Nothing> Connection::end_write() {
  if (PQputCopyEnd(conn_.ptr(), NULL) == -1) {
    return error(PQerrorMessage(conn_.ptr()));
  }
  return PostgresV2Result::make(PQgetResult(conn_.ptr()))
      .and_then([&](auto&& res) -> Result<Nothing> {
        if (PQresultStatus(res.ptr()) != PGRES_COMMAND_OK) {
          return error(PQerrorMessage(conn_.ptr()));
        }
        return Nothing{};
      });
}

std::list<Notification> Connection::get_notifications() noexcept {
  std::list<Notification> notices;

  // Safe to call even if no data — just returns true
  if (!PQconsumeInput(conn_.ptr())) {
    // Note: In pure wait/consume pattern, this should rarely happen if socket
    // is healthy But we don't error here — just skip
    return notices;
  }

  PGnotify* notify;
  while ((notify = PQnotifies(conn_.ptr())) != nullptr) {
    notices.push_back(
        {.channel = std::string(notify->relname),
         .payload = notify->extra[0] ? std::string(notify->extra) : "",
         .backend_pid = notify->be_pid});
    PQfreemem(notify);
  }

  return notices;
}

rfl::Result<Nothing> Connection::listen(const std::string& channel) noexcept {
  if (!is_valid_channel_name(channel)) {
    return error("Invalid channel name: must be a PostgreSQL identifier");
  }
  const std::string sql = "LISTEN " + channel;
  return execute(sql);
}

rfl::Result<Nothing> Connection::unlisten(const std::string& channel) noexcept {
  if (channel == "*") {
    return execute("UNLISTEN *");
  }
  if (!is_valid_channel_name(channel)) {
    return error("Invalid channel name");
  }
  const std::string sql = "UNLISTEN " + channel;
  return execute(sql);
}

rfl::Result<Nothing> Connection::notify(const std::string& channel,
                                        const std::string& payload) noexcept {
  if (!is_valid_channel_name(channel)) {
    return error("Invalid channel name");
  }

  auto* escaped_payload =
      PQescapeLiteral(conn_.ptr(), payload.c_str(), payload.size());
  if (!escaped_payload) {
    return error("Failed to escape NOTIFY payload");
  }
  const std::string sql =
      "NOTIFY " + channel + ", " + std::string(escaped_payload);
  PQfreemem(escaped_payload);

  auto result = execute(sql);
  PQflush(conn_.ptr());
  return result;
}

bool Connection::consume_input() noexcept {
  return PQconsumeInput(conn_.ptr()) == 1;
}

Result<Nothing> Connection::insert_impl(
    const dynamic::Insert& _stmt,
    const std::vector<std::vector<std::optional<std::string>>>&
        _data) noexcept {
  if (_data.size() == 0) {
    return Nothing{};
  }

  const auto name = "sqlgen_insert_into_table_" + internal::random();

  const auto sql = to_sql_impl(_stmt);

  return PostgresV2Result::make(PQprepare(conn_.ptr(), name.c_str(),
                                          sql.c_str(), _data.at(0).size(),
                                          nullptr))
      .and_then([&](auto&& res) -> Result<Nothing> {
        const auto status = PQresultStatus(res.ptr());

        if (status != PGRES_COMMAND_OK) {
          return error("Generating prepared statement for '" + sql +
                       "' failed: " + PQresultErrorMessage(res.ptr()));
        }

        std::vector<const char*> current_row(_data[0].size());

        const int n_params = static_cast<int>(current_row.size());

        for (size_t i = 0; i < _data.size(); ++i) {
          const auto& d = _data[i];

          if (d.size() != current_row.size()) {
            execute("DEALLOCATE " + name + ";");
            return error("Error in entry " + std::to_string(i) + ": Expected " +
                         std::to_string(current_row.size()) + " entries, got " +
                         std::to_string(d.size()));
          }

          for (size_t j = 0; j < d.size(); ++j) {
            current_row[j] = d[j] ? d[j]->c_str() : nullptr;
          }

          try {
            const auto res = PostgresV2Result(PQexecPrepared(
                conn_.ptr(),         // conn
                name.c_str(),        // stmtName
                n_params,            // nParams
                current_row.data(),  // paramValues
                nullptr,             // paramLengths
                nullptr,             // paramFormats
                0                    // resultFormat
                ));

            const auto status = PQresultStatus(res.ptr());

            if (status != PGRES_COMMAND_OK) {
              const auto err = error(std::string("Executing INSERT failed: ") +
                                     PQresultErrorMessage(res.ptr()));
              execute("DEALLOCATE " + name + ";");
              return err;
            }
          } catch (const std::exception& e) {
            const auto err =
                error(std::string("Executing INSERT failed: ") + e.what());
            execute("DEALLOCATE " + name + ";");
            return err;
          }
        }

        return execute("DEALLOCATE " + name + ";");
      });
}

rfl::Result<Ref<Connection>> Connection::make(
    const Credentials& _credentials) noexcept {
  return PostgresV2Connection::make(_credentials.to_str(),
                                    _credentials.notice_handler)
      .transform([](auto&& _conn) { return Ref<Connection>::make(_conn); });
}

Result<Ref<Iterator>> Connection::read_impl(
    const rfl::Variant<dynamic::SelectFrom, dynamic::Union>& _query) {
  const auto sql = _query.visit([](const auto& _q) { return to_sql_impl(_q); });
  return Iterator::make(sql, conn_);
}

Result<Nothing> Connection::rollback() noexcept { return execute("ROLLBACK;"); }

std::string Connection::to_buffer(
    const std::vector<std::optional<std::string>>& _line) const noexcept {
  using namespace std::ranges::views;

  const auto edit_field =
      [](const std::optional<std::string>& _field) -> std::string {
    if (!_field) {
      return "\e";
    }
    if (_field->find("\t") != std::string::npos) {
      return "\a" + *_field + "\a";
    }
    return *_field;
  };

  return internal::strings::join(
             "\t", internal::collect::vector(_line | transform(edit_field))) +
         "\n";
}

std::string Connection::to_sql(const dynamic::Statement& _stmt) noexcept {
  return postgres::to_sql_impl(_stmt);
}

Result<Nothing> Connection::start_write(const dynamic::Write& _stmt) {
  return execute(postgres::to_sql_impl(_stmt));
}

Result<Nothing> Connection::write_impl(
    const std::vector<std::vector<std::optional<std::string>>>& _data) {
  for (const auto& line : _data) {
    const auto buffer = to_buffer(line);
    const auto success = PQputCopyData(conn_.ptr(), buffer.c_str(),
                                       static_cast<int>(buffer.size()));
    if (success != 1) {
      PQputCopyEnd(conn_.ptr(), NULL);
      return error("Error occurred while writing data to postgres.");
    }
  }
  return Nothing{};
}

bool Connection::is_valid_channel_name(const std::string& s) const noexcept {
  if (s.empty()) return false;
  const char first = s[0];
  if (first != '_' && !std::isalpha(static_cast<unsigned char>(first)))
    return false;
  return std::all_of(s.begin() + 1, s.end(), [](char c) {
    return c == '_' || std::isalnum(static_cast<unsigned char>(c));
  });
}

}  // namespace sqlgen::postgres
