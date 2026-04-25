#ifndef SQLGEN_SQLITE_TO_SQL_HPP_
#define SQLGEN_SQLITE_TO_SQL_HPP_

#include <string>

#include "../dynamic/Statement.hpp"
#include "../sqlgen_api.hpp"
#include "../transpilation/to_sql.hpp"

namespace sqlgen::sqlite {

/// Transpiles a dynamic general SQL statement to the sqlite dialect.
std::string SQLGEN_API to_sql_impl(const dynamic::Statement& _stmt) noexcept;

/// Transpiles any  SQL statement to the sqlite dialect.
template <class T>
std::string to_sql(const T& _t) noexcept {
  if constexpr (std::is_same_v<std::remove_cvref_t<T>, dynamic::Statement>) {
    return to_sql_impl(_t);
  } else {
    return to_sql_impl(transpilation::to_sql(_t));
  }
}

}  // namespace sqlgen::sqlite

#endif
