#include "sqlgen/postgres/PostgresV2Connection.hpp"

namespace sqlgen::postgres {

namespace {
void notice_trampoline(void* arg, const char* message) {
  auto* handler =
      static_cast<PostgresV2Connection::NoticeHandler*>(arg);
  if (handler && *handler) {
    (*handler)(message);
  }
}
}  // namespace

PostgresV2Connection::PostgresV2Connection(PGconn* _ptr,
                                           NoticeHandler _notice_handler)
    : ptr_(Ref<PGconn>::make(std::shared_ptr<PGconn>(_ptr, &PQfinish)).value()),
      notice_handler_(_notice_handler
                          ? std::make_shared<NoticeHandler>(
                                std::move(_notice_handler))
                          : nullptr) {
  if (notice_handler_) {
    PQsetNoticeProcessor(ptr_.get(), notice_trampoline, notice_handler_.get());
  }
}

rfl::Result<PostgresV2Connection> PostgresV2Connection::make(
    const std::string& _conn_str) noexcept {
  auto conn = PQconnectdb(_conn_str.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    const auto msg =
        std::string("Connection to postgres failed: ") + PQerrorMessage(conn);
    PQfinish(conn);
    return error(msg);
  }
  return PostgresV2Connection(conn);
}

rfl::Result<PostgresV2Connection> PostgresV2Connection::make(
    const std::string& _conn_str, NoticeHandler _notice_handler) noexcept {
  auto conn = PQconnectdb(_conn_str.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    const auto msg =
        std::string("Connection to postgres failed: ") + PQerrorMessage(conn);
    PQfinish(conn);
    return error(msg);
  }
  return PostgresV2Connection(conn, std::move(_notice_handler));
}

}  // namespace sqlgen::postgres
