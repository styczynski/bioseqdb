#include "sqlgen/postgres/PostgresV2Connection.hpp"
#include "sqlgen/postgres/PostgresV2Result.hpp"

namespace sqlgen::postgres {

rfl::Result<PostgresV2Result> PostgresV2Result::make(
    const std::string& _query, const PostgresV2Connection& _conn) noexcept {
  auto res = PQexec(_conn.ptr(), _query.c_str());
  const auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK &&
      status != PGRES_COPY_IN) {
    const auto msg =
        std::string("Query execution failed: ") + PQerrorMessage(_conn.ptr());
    PQclear(res);
    return error(msg);
  }
  return PostgresV2Result(res);
}

rfl::Result<PostgresV2Result> PostgresV2Result::make(
    const std::string& _query, const PostgresV2Connection& _conn,
    const std::vector<std::optional<std::string>>& _params) noexcept {
  std::vector<const char*> param_values(_params.size());
  for (size_t i = 0; i < _params.size(); ++i) {
    param_values[i] = _params[i] ? _params[i]->c_str() : nullptr;
  }

  auto res = PQexecParams(_conn.ptr(), _query.c_str(),
                          static_cast<int>(_params.size()),
                          nullptr,             // paramTypes (let server infer)
                          param_values.data(), // paramValues
                          nullptr,             // paramLengths (text format)
                          nullptr,             // paramFormats (text format)
                          0);                  // resultFormat (text)

  const auto status = PQresultStatus(res);
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK &&
      status != PGRES_COPY_IN) {
    const auto msg =
        std::string("Query execution failed: ") + PQerrorMessage(_conn.ptr());
    PQclear(res);
    return error(msg);
  }
  return PostgresV2Result(res);
}

}  // namespace sqlgen::postgres
