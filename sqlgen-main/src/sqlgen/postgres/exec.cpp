#include "sqlgen/postgres/exec.hpp"

#include <ranges>
#include <rfl.hpp>
#include <sstream>
#include <stdexcept>

namespace sqlgen::postgres {

Result<PostgresV2Result> exec(const PostgresV2Connection& _conn,
                              const std::string& _sql) noexcept {
  return PostgresV2Result::make(_sql, _conn);
}

}  // namespace sqlgen::postgres
