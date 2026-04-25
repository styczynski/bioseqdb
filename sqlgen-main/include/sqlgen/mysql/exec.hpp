#ifndef SQLGEN_MYSQL_EXEC_HPP_
#define SQLGEN_MYSQL_EXEC_HPP_

#include <mysql.h>

#include <string>

#include "../Ref.hpp"
#include "../Result.hpp"
#include "../sqlgen_api.hpp"

namespace sqlgen::mysql {

Result<Nothing> SQLGEN_API exec(const Ref<MYSQL>& _conn,
                                const std::string& _sql) noexcept;

}  // namespace sqlgen::mysql

#endif
