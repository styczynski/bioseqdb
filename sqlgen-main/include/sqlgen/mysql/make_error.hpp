#ifndef SQLGEN_MYSQL_MAKE_ERROR_HPP_
#define SQLGEN_MYSQL_MAKE_ERROR_HPP_

#include <mysql.h>

#include <rfl.hpp>
#include <string>

#include "../Ref.hpp"
#include "../Result.hpp"

namespace sqlgen::mysql {

inline rfl::Unexpected<Error> make_error(const Ref<MYSQL>& _conn) noexcept {
  return error("MySQL error (" + std::to_string(mysql_errno(_conn.get())) +
               ") [" + mysql_sqlstate(_conn.get()) + "] " +
               mysql_error(_conn.get()));
}

}  // namespace sqlgen::mysql

#endif
