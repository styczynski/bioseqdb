#ifndef SQLGEN_POSTGRES_EXEC_HPP_
#define SQLGEN_POSTGRES_EXEC_HPP_

#include <libpq-fe.h>

#include <rfl.hpp>
#include <string>

#include "../Ref.hpp"
#include "../Result.hpp"
#include "../sqlgen_api.hpp"
#include "PostgresV2Connection.hpp"
#include "PostgresV2Result.hpp"

namespace sqlgen::postgres {

Result<PostgresV2Result> SQLGEN_API exec(const PostgresV2Connection& _conn,
                                         const std::string& _sql) noexcept;

}  // namespace sqlgen::postgres

#endif
