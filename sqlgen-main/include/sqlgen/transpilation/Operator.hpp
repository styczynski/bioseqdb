#ifndef SQLGEN_TRANSPILATION_OPERATOR_HPP_
#define SQLGEN_TRANSPILATION_OPERATOR_HPP_

namespace sqlgen::transpilation {

enum class Operator {
  abs,
  cast,
  ceil,
  coalesce,
  concat,
  cos,
  date_plus_duration,
  day,
  days_between,
  divides,
  exp,
  floor,
  hour,
  length,
  ln,
  log2,
  lower,
  ltrim,
  minus,
  minute,
  mod,
  month,
  multiplies,
  plus,
  replace,
  round,
  rtrim,
  second,
  sin,
  sqrt,
  tan,
  trim,
  unixepoch,
  upper,
  weekday,
  year
};

}  // namespace sqlgen::transpilation

#endif
