#ifndef SQLGEN_DUCKDB_PARSING_PARSER_ENUM_HPP_
#define SQLGEN_DUCKDB_PARSING_PARSER_ENUM_HPP_

#include <duckdb.h>

#include <rfl.hpp>
#include <rfl/enums.hpp>
#include <string>
#include <type_traits>

#include "../../Result.hpp"
#include "Parser_base.hpp"

namespace sqlgen::duckdb::parsing {

template <class EnumT>
  requires std::is_enum_v<EnumT>
struct Parser<EnumT> {
  using ResultingType = uint8_t;

  static_assert(enchantum::ScopedEnum<EnumT>, "The enum must be scoped.");
  static constexpr auto arr = rfl::get_enumerator_array<EnumT>();
  static_assert(arr.size() < 255, "Enum size cannot exceed 255.");

  static Result<EnumT> read(const ResultingType* _r) noexcept {
    if (!_r) {
      return error("Enum value cannot be NULL.");
    }
    return static_cast<EnumT>(*_r);
  }

  static Result<Nothing> write(const EnumT& _t,
                               duckdb_appender _appender) noexcept {
    const auto str = rfl::enum_to_string(_t);
    return duckdb_append_varchar_length(_appender, str.c_str(), str.length()) !=
                   DuckDBError
               ? Result<Nothing>(Nothing{})
               : Result<Nothing>(error("Could not append string value."));
  }
};

}  // namespace sqlgen::duckdb::parsing

#endif
